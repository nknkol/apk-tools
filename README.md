# apk-tools

Alpine Package Keeper (apk) is a package manager originally built for Alpine Linux,
but now used by several other distributions as well.

## Building

The preferred build system for building apk-tools is Meson. By default, builds
install under the Meson prefix (e.g. `/usr/local` unless overridden):

```
$ meson setup build
$ ninja -C build
$ meson install -C build
```

To install elsewhere, override the prefix when configuring, for example
`-Dprefix=/usr`.

运行时，`apk` 的默认根目录（`--root`）指向 `$HOME/.hapkg/sysroot`；如需
操作系统根，显式传入 `--root /`。

安装 ELF 可执行文件时，会为每个可执行生成 shim，shim 会设置 `HPKG_PREFIX`、
`LD_LIBRARY_PATH` 并通过 `loader` 调用实际位于 `$HOME/.hapkg/sysroot` 下的二
进制；所有 shim 会被打包到 `$HOME/.hapkg/temp/horpkgruntime.hnp`（内容来自
`$HOME/.hapkg/temp/horpkgruntime/bin`）。

初次自动初始化会写入 HTTP 的 main/community 仓库地址。公钥和 CA 证书需要在
构建时或手动放入 `~/.hapkg/sysroot/etc/apk/keys/` 与 `~/.hapkg/sysroot/etc/ssl/certs/`
中，必要时再切换回 HTTPS（编辑 `~/.hapkg/sysroot/etc/apk/repositories`）。

首次在默认根下使用时会自动初始化 `~/.hapkg/sysroot`（创建数据库目录、写入
`main`/`community` 仓库并尝试获取官方公钥）；如需手动初始化，可运行
`apk add --root ~/.hapkg/sysroot --initdb --usermode`。

For bootstrapping without Python, muon is also compatible. All you have to do is replace `meson` with `muon` in the above example.

To build a static apk, pass the right arguments to the above commands:

```
# meson setup -Dc_link_args="-static" -Dprefer_static=true -Ddefault_library=static build
# ninja -C build src/apk
```

Which will give you a `./build/src/apk` that is statically linked.

While there is a legacy Makefile-based system available, it only works for musl-linux
targets, and will be dropped in the apk-tools 3.0 release.

## Documentation

Online documentation is available in the [doc/](doc/) directory in the form of man pages.

The [apk(8)](doc/apk.8.scd) man page provides a basic overview of the package management
system.
