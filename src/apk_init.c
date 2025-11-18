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

// Use http to avoid TLS bootstrap issues when no CA certs are present.
#define HAPKG_REPO_BASE "http://dl-cdn.alpinelinux.org/alpine/latest-stable"
#define HAPKG_KEYS_BASE "http://git.alpinelinux.org/aports/plain/main/alpine-keys"
#define HAPKG_CA_URL    "http://curl.se/ca/cacert.pem"

static const char *default_repos[] = {
	HAPKG_REPO_BASE "/main",
	HAPKG_REPO_BASE "/community",
};

static const char *default_keys[] = {
	"alpine-devel@lists.alpinelinux.org-4a6a0840.rsa.pub",
	"alpine-devel@lists.alpinelinux.org-58cbb476.rsa.pub",
	"alpine-devel@lists.alpinelinux.org-524d27bb.rsa.pub",
	"alpine-devel@lists.alpinelinux.org-5e69ca50.rsa.pub",
	"alpine-devel@lists.alpinelinux.org-6165ee59.rsa.pub",
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

static int download_to_file(struct apk_ctx *ac, const char *url, const char *path)
{
	struct apk_out *out = &ac->out;
	struct apk_istream *is = apk_io_url_istream(url, APK_ISTREAM_FORCE_REFRESH);
	if (IS_ERR(is)) return PTR_ERR(is);

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		apk_istream_close(is);
		return -errno;
	}

	char buf[4096];
	int r = 0;
	while ((r = apk_istream_read(is, buf, sizeof buf)) > 0) {
		if (apk_write_fully(fd, buf, r) != r) {
			r = -errno;
			break;
		}
	}
	int cr = apk_istream_close(is);
	if (r >= 0 && cr < 0) r = cr;
	close(fd);

	if (r < 0) {
		unlink(path);
		apk_warn(out, "Failed to download %s: %s", url, apk_error_str(r));
		return r;
	}

	return 0;
}

static int ensure_keys(struct apk_ctx *ac, const char *keys_dir)
{
	struct apk_out *out = &ac->out;
	char key_path[PATH_MAX], key_url[PATH_MAX];
	int r = ensure_dir(keys_dir);
	if (r < 0) return r;

	for (size_t i = 0; i < ARRAY_SIZE(default_keys); i++) {
		if (path_join(key_path, sizeof key_path, keys_dir, default_keys[i]) < 0)
			continue;
		if (access(key_path, F_OK) == 0)
			continue;
		if (apk_fmt(key_url, sizeof key_url, "%s/%s", HAPKG_KEYS_BASE, default_keys[i]) < 0)
			continue;
		r = download_to_file(ac, key_url, key_path);
		if (r == 0)
			apk_msg(out, "Downloaded key %s", default_keys[i]);
	}
	return 0;
}

static int ensure_ca_bundle(struct apk_ctx *ac, const char *root)
{
	char ca_dir[PATH_MAX], ca_path[PATH_MAX];

	if (apk_fmt(ca_dir, sizeof ca_dir, "%s/etc/ssl/certs", root) < 0) return -ENAMETOOLONG;
	if (apk_fmt(ca_path, sizeof ca_path, "%s/ca-certificates.crt", ca_dir) < 0) return -ENAMETOOLONG;

	if (ensure_dir(ca_dir) < 0) return -errno;
	if (access(ca_path, F_OK) == 0) return 0;
	return download_to_file(ac, HAPKG_CA_URL, ca_path);
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

	if (apk_fmt(path, sizeof path, "%s/etc/apk/keys", root) >= 0)
		ensure_keys(ac, path);
	ensure_ca_bundle(ac, root);

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
