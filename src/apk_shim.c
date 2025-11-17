/* apk_shim.c - create runtime shims for installed executables
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
#include <sys/stat.h>
#include <unistd.h>

#include "apk_shim.h"
#include "apk_io.h"
#include "apk_print.h"

static bool is_executable_elf(int root_fd, const char *relpath)
{
	struct stat st;
	unsigned char magic[4];

	if (fstatat(root_fd, relpath, &st, AT_SYMLINK_NOFOLLOW) != 0) return false;
	if (!S_ISREG(st.st_mode)) return false;
	if ((st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) return false;

	int fd = openat(root_fd, relpath, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return false;
	ssize_t r = read(fd, magic, sizeof magic);
	close(fd);
	if (r != sizeof magic) return false;
	return magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

static int get_hpkg_prefix(struct apk_ctx *ac, char *buf, size_t bufsz)
{
	const char *env = getenv("HPKG_PREFIX");
	const char *root = ac->root;
	const char suffix[] = "/sysroot";

	if (env && env[0]) {
		if (strlcpy(buf, env, bufsz) >= bufsz) return -ENAMETOOLONG;
		return 0;
	}

	if (!root || !root[0]) root = "/";
	size_t rootlen = strnlen(root, PATH_MAX);
	if (rootlen > sizeof suffix - 1 && strcmp(root + rootlen - (sizeof suffix - 1), suffix) == 0)
		rootlen -= sizeof suffix - 1;

	if (rootlen == 0) {
		const char *home = getenv("HOME");
		if (home && home[0]) {
			if (snprintf(buf, bufsz, "%s/.horpkg", home) >= (int)bufsz) return -ENAMETOOLONG;
			return 0;
		}
		root = "/";
		rootlen = 1;
	}

	if (rootlen >= bufsz) return -ENAMETOOLONG;
	memcpy(buf, root, rootlen);
	buf[rootlen] = 0;
	return 0;
}

static int build_paths(struct apk_ctx *ac, const char *relpath, char *runtime, size_t runtimesz, char *realbin, size_t realbinsz)
{
	char prefix[PATH_MAX];
	const char *p = relpath;
	while (*p == '/') p++;
	if (!*p) return -EINVAL;

	int r = get_hpkg_prefix(ac, prefix, sizeof prefix);
	if (r != 0) return r;

	if (snprintf(runtime, runtimesz, "%s/runtime/%s", prefix, p) >= (int)runtimesz) return -ENAMETOOLONG;
	if (snprintf(realbin, realbinsz, "%s/sysroot/%s", prefix, p) >= (int)realbinsz) return -ENAMETOOLONG;
	return 0;
}

static int ensure_dir_for(const char *path)
{
	char dir[PATH_MAX];
	char *slash;

	if (strlcpy(dir, path, sizeof dir) >= sizeof dir) return -ENAMETOOLONG;
	slash = strrchr(dir, '/');
	if (!slash) return -EINVAL;
	*slash = 0;
	if (dir[0] == 0) return 0;
	if (apk_make_dirs(AT_FDCWD, dir, 0755, 0755) < 0 && errno != EEXIST) return -errno;
	return 0;
}

static int write_shim(const char *shim_path, const char *real_path)
{
	static const char header[] = "#!/system/bin/sh\n";
	int fd = open(shim_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
	if (fd < 0) return -errno;

	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		return -errno;
	}

	const char *template =
		"HPKG_PREFIX=\"${HPKG_PREFIX:-$HOME/.horpkg}\"\n"
		"REAL_BIN=\"%s\"\n"
		"\n"
		"export HPKG_PREFIX\n"
		"if [ -d \"$HPKG_PREFIX/sysroot/lib\" ]; then\n"
		"    if [ -n \"$LD_LIBRARY_PATH\" ]; then\n"
		"        LD_LIBRARY_PATH=\"$HPKG_PREFIX/sysroot/lib:$LD_LIBRARY_PATH\"\n"
		"    else\n"
		"        LD_LIBRARY_PATH=\"$HPKG_PREFIX/sysroot/lib\"\n"
		"    fi\n"
		"fi\n"
		"if [ -d \"$HPKG_PREFIX/sysroot/usr/lib\" ]; then\n"
		"    if [ -n \"$LD_LIBRARY_PATH\" ]; then\n"
		"        LD_LIBRARY_PATH=\"$HPKG_PREFIX/sysroot/usr/lib:$LD_LIBRARY_PATH\"\n"
		"    else\n"
		"        LD_LIBRARY_PATH=\"$HPKG_PREFIX/sysroot/usr/lib\"\n"
		"    fi\n"
		"fi\n"
		"if [ -n \"$LD_LIBRARY_PATH\" ]; then\n"
		"    export LD_LIBRARY_PATH\n"
		"fi\n"
		"\n"
		"exec loader \"$REAL_BIN\" \"$@\"\n";

	int err = 0;
	if (fwrite(header, 1, sizeof header - 1, f) != sizeof header - 1) err = -errno;
	if (!err && fprintf(f, template, real_path) < 0) err = -errno;
	if (fclose(f) != 0 && !err) err = -errno;
	return err;
}

int apk_shim_install(struct apk_ctx *ac, const char *relative_path)
{
	char runtime_path[PATH_MAX], real_path[PATH_MAX];
	int r;

	if (!is_executable_elf(apk_ctx_fd_root(ac), relative_path))
		return 0;

	r = build_paths(ac, relative_path, runtime_path, sizeof runtime_path, real_path, sizeof real_path);
	if (r != 0) return r;

	r = ensure_dir_for(runtime_path);
	if (r != 0) return r;

	return write_shim(runtime_path, real_path);
}

int apk_shim_remove(struct apk_ctx *ac, const char *relative_path)
{
	char runtime_path[PATH_MAX], real_path[PATH_MAX];
	int r = build_paths(ac, relative_path, runtime_path, sizeof runtime_path, real_path, sizeof real_path);
	if (r != 0) return r;

	if (unlink(runtime_path) < 0 && errno != ENOENT) return -errno;
	return 0;
}
