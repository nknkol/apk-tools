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
#include <sys/types.h>
#include <unistd.h>

#include "apk_shim.h"
#include "apk_io.h"
#include "apk_process.h"
#include "apk_print.h"

#define HNPROOT_NAME "horpkgruntime"
#define HNP_OUTPUT_NAME "horpkgruntime.hnp"
#define BASE_RUNTIME_HAP "/data/service/hnp/horpkg-base.org/horpkg-base_1.0/share/horpkg/resources/org.horpkg.runtime.hap"
#define HNP_PIN "314159"

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

static bool is_shim_target(const char *relpath)
{
	// Only create shims for typical executable locations.
	static const char *bins[] = {
		"bin/",
		"sbin/",
		"usr/bin/",
		"usr/sbin/",
		"usr/local/bin/",
	};
	for (size_t i = 0; i < ARRAY_SIZE(bins); i++) {
		size_t len = strlen(bins[i]);
		if (strncmp(relpath, bins[i], len) == 0) return true;
	}
	return false;
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
			if (snprintf(buf, bufsz, "%s/.hapkg", home) >= (int)bufsz) return -ENAMETOOLONG;
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

static int build_paths(struct apk_ctx *ac, const char *relpath, char *stage_root, size_t stage_root_sz, char *shim_path, size_t shim_sz, char *realbin, size_t realbinsz)
{
	char prefix[PATH_MAX];
	const char *p = relpath;
	const char *basename = strrchr(relpath, '/');
	if (!basename) basename = relpath;
	else basename++;

	while (*p == '/') p++;
	if (!*p || !*basename) return -EINVAL;

	int r = get_hpkg_prefix(ac, prefix, sizeof prefix);
	if (r != 0) return r;

	if (snprintf(stage_root, stage_root_sz, "%s/temp/%s", prefix, HNPROOT_NAME) >= (int)stage_root_sz) return -ENAMETOOLONG;

	if (snprintf(shim_path, shim_sz, "%s/bin/%s", stage_root, basename) >= (int)shim_sz) return -ENAMETOOLONG;
	if (snprintf(realbin, realbinsz, "%s/sysroot/%s", prefix, p) >= (int)realbinsz) return -ENAMETOOLONG;
	return 0;
}

static int ensure_parent_dir(const char *path)
{
	char dir[PATH_MAX];
	char *slash;

	if (strlcpy(dir, path, sizeof dir) >= sizeof dir) return -ENAMETOOLONG;
	slash = strrchr(dir, '/');
	if (!slash) return 0;
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
			"HPKG_PREFIX=\"${HPKG_PREFIX:-$HOME/.hapkg}\"\n"
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

static int run_process(struct apk_ctx *ac, char * const*argv, const char *name)
{
	struct apk_process p;
	int r = apk_process_init(&p, name, &ac->out, NULL);
	if (r != 0) return r;
	r = apk_process_spawn(&p, argv[0], argv, NULL);
	if (r != 0) return r;
	r = apk_process_run(&p);
	r = apk_process_cleanup(&p);
	return r;
}

static int copy_file(const char *src, const char *dst)
{
	int r, sfd = open(src, O_RDONLY | O_CLOEXEC);
	if (sfd < 0) return -errno;

	r = ensure_parent_dir(dst);
	if (r < 0) {
		close(sfd);
		return r;
	}

	int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (dfd < 0) {
		r = -errno;
		close(sfd);
		return r;
	}

	char buf[8192];
	ssize_t n;
	while ((n = read(sfd, buf, sizeof buf)) > 0) {
		if (apk_write_fully(dfd, buf, n) != n) {
			r = -errno;
			close(sfd);
			close(dfd);
			return r;
		}
	}
	if (n < 0) r = -errno; else r = 0;
	close(sfd);
	close(dfd);
	return r;
}

static int reset_dir(struct apk_ctx *ac, const char *path)
{
	char *rm_argv[] = { "rm", "-rf", (char*)path, NULL };
	run_process(ac, rm_argv, "rm");
	if (apk_make_dirs(AT_FDCWD, path, 0755, 0755) < 0 && errno != EEXIST)
		return -errno;
	return 0;
}

int apk_shim_install(struct apk_ctx *ac, const char *relative_path)
{
	char stage_root[PATH_MAX], shim_path[PATH_MAX], real_path[PATH_MAX];
	char bin_dir[PATH_MAX], manifest[PATH_MAX];
	int r;

	if (!is_shim_target(relative_path)) return 0;
	if (!is_executable_elf(apk_ctx_fd_root(ac), relative_path))
		return 0;

	r = build_paths(ac, relative_path, stage_root, sizeof stage_root, shim_path, sizeof shim_path, real_path, sizeof real_path);
	if (r != 0) return r;

	if (apk_make_dirs(AT_FDCWD, stage_root, 0755, 0755) < 0 && errno != EEXIST) return -errno;
	if (snprintf(bin_dir, sizeof bin_dir, "%s/bin", stage_root) >= (int)sizeof bin_dir) return -ENAMETOOLONG;
	if (apk_make_dirs(AT_FDCWD, bin_dir, 0755, 0755) < 0 && errno != EEXIST) return -errno;

	if (snprintf(manifest, sizeof manifest, "%s/hnp.json", stage_root) >= (int)sizeof manifest) return -ENAMETOOLONG;
	struct stat st;
	if (stat(manifest, &st) != 0) {
		int fd = open(manifest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd >= 0) {
			dprintf(fd,
				"{\n"
				"  \"type\": \"hnp-config\",\n"
				"  \"name\": \"%s\",\n"
				"  \"version\": \"1.0\",\n"
				"  \"install\": {\n"
				"    \"links\": []\n"
				"  }\n"
				"}\n",
				HNPROOT_NAME);
			close(fd);
		}
	}

	r = write_shim(shim_path, real_path);
	if (r != 0) return r;

	ac->shim_dirty = 1;
	return 0;
}

int apk_shim_remove(struct apk_ctx *ac, const char *relative_path)
{
	char stage_root[PATH_MAX], shim_path[PATH_MAX], real_path[PATH_MAX];
	int r = build_paths(ac, relative_path, stage_root, sizeof stage_root, shim_path, sizeof shim_path, real_path, sizeof real_path);
	if (r != 0) return r;

	if (unlink(shim_path) < 0 && errno != ENOENT) return -errno;
	ac->shim_dirty = 1;
	return 0;
}

int apk_shim_pack(struct apk_ctx *ac)
{
	if (getenv("HPKG_SKIP_SHIM_PACK")) return 0;
	if (!ac->shim_dirty) return 0;

	char prefix[PATH_MAX], stage_root[PATH_MAX], output_dir[PATH_MAX], hap_dest[PATH_MAX], new_hnp[PATH_MAX], hap_hnp[PATH_MAX];
	int r = get_hpkg_prefix(ac, prefix, sizeof prefix);
	if (r != 0) return 0;

	if (snprintf(stage_root, sizeof stage_root, "%s/temp/%s", prefix, HNPROOT_NAME) >= (int)sizeof stage_root)
		return -ENAMETOOLONG;
	if (snprintf(output_dir, sizeof output_dir, "%s/temp", prefix) >= (int)sizeof output_dir)
		return -ENAMETOOLONG;

	struct stat st;
	if (stat(stage_root, &st) != 0) {
		ac->shim_dirty = 0;
		return 0;
	}

	if (apk_make_dirs(AT_FDCWD, output_dir, 0755, 0755) < 0 && errno != EEXIST)
		return -errno;

	char *argv[] = { "hnpcli", "pack", "-i", stage_root, "-o", output_dir, NULL };
	r = run_process(ac, argv, "hnpcli");
	if (r != 0) return r;

	if (snprintf(new_hnp, sizeof new_hnp, "%s/%s", output_dir, HNP_OUTPUT_NAME) >= (int)sizeof new_hnp)
		return -ENAMETOOLONG;

	if (snprintf(hap_dest, sizeof hap_dest, "%s/org.horpkg.runtime.hap", output_dir) >= (int)sizeof hap_dest)
		return -ENAMETOOLONG;
	if (snprintf(hap_hnp, sizeof hap_hnp, "%s/org.horpkg.runtime.hap.dir", output_dir) >= (int)sizeof hap_hnp)
		return -ENAMETOOLONG;

	r = reset_dir(ac, hap_hnp);
	if (r != 0) return r;

	char *unzip_argv[] = { "unzip", "-oq", (char*)BASE_RUNTIME_HAP, "-d", hap_hnp, NULL };
	r = run_process(ac, unzip_argv, "unzip");
	if (r != 0) return r;

	char hap_hnp_target[PATH_MAX], hap_hnp_dir[PATH_MAX];
	if (apk_fmt(hap_hnp_target, sizeof hap_hnp_target, "%s/hnp/arm64-v8a/%s", hap_hnp, HNP_OUTPUT_NAME) < 0)
		return -ENAMETOOLONG;
	if (apk_fmt(hap_hnp_dir, sizeof hap_hnp_dir, "%s/hnp/arm64-v8a", hap_hnp) < 0)
		return -ENAMETOOLONG;
	if (apk_make_dirs(AT_FDCWD, hap_hnp_dir, 0755, 0755) < 0 && errno != EEXIST)
		return -errno;
	r = copy_file(new_hnp, hap_hnp_target);
	if (r != 0) return r;

	// Repack hap
	char zip_cmd[PATH_MAX * 2];
	if (apk_fmt(zip_cmd, sizeof zip_cmd, "cd %s && zip -qr %s .", hap_hnp, hap_dest) < 0)
		return -ENAMETOOLONG;
	char *zip_argv[] = { "sh", "-c", zip_cmd, NULL };
	char *rm_hap[] = { "rm", "-f", hap_dest, NULL };
	run_process(ac, rm_hap, "rm");
	r = run_process(ac, zip_argv, "zip");
	if (r != 0) return r;

	char *install_argv[] = { "horpkg", "install", "pin", HNP_PIN, hap_dest, NULL };
	r = run_process(ac, install_argv, "horpkg");
	if (r == 0) ac->shim_dirty = 0;

	return r;
}
