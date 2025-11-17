# apk-tools

Alpine Package Keeper (apk) is a package manager originally built for Alpine Linux,
but now used by several other distributions as well.

## Building

The preferred build system for building apk-tools is Meson. By default, builds
install under `$HOME/.horpkg/sysroot`:

```
$ meson setup build
$ ninja -C build
$ meson install -C build
```

To install elsewhere, override the prefix when configuring, for example
`-Dprefix=/usr`.

运行时，`apk` 的默认根目录（`--root`）同样指向 `$HOME/.horpkg/sysroot`；
如需操作系统根，显式传入 `--root /`。

安装 ELF 可执行文件时，会自动在 `$HOME/.horpkg/runtime` 下生成同路径的 shim
脚本，shim 会设置 `HPKG_PREFIX`、`LD_LIBRARY_PATH` 并通过 `loader` 调用实际
位于 `$HOME/.horpkg/sysroot` 下的可执行文件。

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
