/* apk_init.c - initialization helpers for default hapkg root
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "apk_init.h"
#include "apk_blob.h"
#include "apk_defines.h"
#include "apk_io.h"
#include "apk_print.h"

// Repositories only; keys/CA are expected to be provided ahead of time (build-time or manual).
#define HAPKG_REPO_BASE "http://dl-cdn.alpinelinux.org/alpine/latest-stable"

static const char *default_repos[] = {
	HAPKG_REPO_BASE "/main",
	HAPKG_REPO_BASE "/community",
};

static int path_join(char *buf, size_t len, const char *a, const char *b)
{
	int r = apk_fmt(buf, len, "%s/%s", a, b);
	if (r < 0) return r;
	return 0;
}

static int ensure_dir(const char *path)
{
	if (apk_make_dirs(AT_FDCWD, path, 0755, 0755) < 0 && errno != EEXIST)
		return -errno;
	return 0;
}

static int write_repositories(struct apk_ctx *ac, const char *path)
{
	struct apk_out *out = &ac->out;
	struct stat st;
	FILE *f;
	int r;
	char dir[PATH_MAX];
	char *slash;

	if (stat(path, &st) == 0 && st.st_size > 0)
		return 0;

	snprintf(dir, sizeof dir, "%s", path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = 0;
		r = ensure_dir(dir);
		if (r < 0 && r != -EEXIST) return r;
	}

	f = fopen(path, "w");
	if (!f) return -errno;
	for (size_t i = 0; i < ARRAY_SIZE(default_repos); i++)
		fprintf(f, "%s\n", default_repos[i]);
	if (fclose(f) != 0) return -errno;

	apk_msg(out, "Initialized repositories at %s", path);
	return 0;
}

static bool hapkg_root(struct apk_ctx *ac, const char *root)
{
	const char *def = apk_ctx_default_root();
	if (!root || !root[0]) return true;
	if (def && strcmp(root, def) == 0) return true;
	return strstr(root, "/.hapkg/sysroot") != NULL;
}

int apk_auto_init(struct apk_ctx *ac, bool *created_db)
{
	char path[PATH_MAX];
	struct apk_out *out = &ac->out;
	const char *root = ac->root;
	struct stat st;

	if (!root || !root[0]) root = apk_ctx_default_root();
	if (!ac->root) ac->root = root;
	if (!hapkg_root(ac, root)) return 0;

	if (ensure_dir(root) < 0)
		return 0;

	if (apk_fmt(path, sizeof path, "%s/etc/apk", root) >= 0)
		ensure_dir(path);
	if (apk_fmt(path, sizeof path, "%s/lib/apk/db", root) >= 0)
		ensure_dir(path);
	if (apk_fmt(path, sizeof path, "%s/var/cache/apk", root) >= 0)
		ensure_dir(path);
	if (apk_fmt(path, sizeof path, "%s/etc/apk/repositories", root) >= 0)
		write_repositories(ac, path);

	// Keys/CA are expected to be provided at build time or manually; no runtime download.

	if (created_db) *created_db = false;
	if (apk_fmt(path, sizeof path, "%s/lib/apk/db/lock", root) >= 0) {
		if (stat(path, &st) != 0) {
			if (getuid() != 0) ac->open_flags |= APK_OPENF_USERMODE;
			ac->open_flags |= APK_OPENF_CREATE;
			if (created_db) *created_db = true;
			apk_msg(out, "Preparing apk database under %s", root);
		}
	}

	return 0;
}
