# 传统Makefile构建

<cite>
**Referenced Files in This Document**   
- [src/Makefile](file://src/Makefile)
- [Makefile](file://Makefile)
- [README.md](file://README.md)
- [Make.rules](file://Make.rules)
- [meson_options.txt](file://meson_options.txt)
</cite>

## 目录
1. [简介](#简介)
2. [构建系统现状](#构建系统现状)
3. [核心变量配置](#核心变量配置)
4. [库文件构建规则](#库文件构建规则)
5. [可执行文件链接过程](#可执行文件链接过程)
6. [编译链接参数设置](#编译链接参数设置)
7. [基本使用示例](#基本使用示例)
8. [局限性与过渡建议](#局限性与过渡建议)

## 简介
传统Makefile构建系统是apk-tools项目早期采用的构建方式，用于编译和链接项目中的C语言源代码，生成动态库、静态库以及核心的apk可执行文件。该系统通过一系列Makefile文件和构建规则，管理源代码的编译、依赖关系和最终产物的生成。

**Section sources**
- [src/Makefile](file://src/Makefile#L1-L126)
- [Makefile](file://Makefile#L1-L65)

## 构建系统现状
apk-tools的传统Makefile构建系统目前处于**弃用状态**。根据项目文档，该构建系统存在明确的局限性：**仅支持musl-linux目标平台**。这意味着它无法在使用glibc等其他C库的Linux发行版或非Linux平台上顺利构建。项目明确指出，此Makefile系统将在**apk-tools 3.0版本中被完全移除**。因此，该构建方式被视为一个临时的、用于特定场景（如在Alpine Linux上进行引导构建）的解决方案，而非长期的构建标准。

**Section sources**
- [README.md](file://README.md#L27-L29)

## 核心变量配置
src/Makefile中的关键变量允许用户在构建时进行配置，以适应不同的环境和需求。

### URL后端选择 (URL_BACKEND)
`URL_BACKEND` 变量决定了用于处理网络请求的后端库。其值在顶层Makefile中通过 `URL_BACKEND ?= libfetch` 设定默认值。
- **`libfetch`**: 选择此值时，构建系统会编译 `io_url_libfetch.c` 源文件，并链接位于 `libfetch/` 目录下的 `libfetch.a` 静态库。同时，编译标志 `CFLAGS_ALL` 会添加 `-Ilibfetch` 以包含其头文件。
- **`wget`**: 选择此值时，构建系统会编译 `io_url_wget.c` 源文件，依赖系统已安装的wget工具进行网络操作，不链接额外的库。

**Section sources**
- [src/Makefile](file://src/Makefile#L1-L7)
- [Makefile](file://Makefile#L39)

### 加密后端选择 (CRYPTO)
`CRYPTO` 变量决定了用于加密操作的后端库。其值在顶层Makefile中通过 `CRYPTO ?= openssl` 设定默认值。
- **`openssl`**: 选择此值时，构建系统会使用 `pkg-config` 工具查询OpenSSL库的编译和链接标志，并将这些标志分别赋值给 `CRYPTO_CFLAGS` 和 `CRYPTO_LIBS`。
- **`mbedtls`**: 选择此值时，构建系统会使用 `pkg-config` 工具查询mbedTLS库的编译和链接标志。

**Section sources**
- [src/Makefile](file://src/Makefile#L9-L15)
- [Makefile](file://Makefile#L40)

### ZSTD支持 (ZSTD)
`ZSTD` 变量用于控制是否启用ZSTD压缩支持。默认情况下，如果未将此变量设为 `no`，则会启用ZSTD。
- 当 `ZSTD` 不等于 `no` 时，构建系统会使用 `pkg-config` 查询 `libzstd` 库的编译和链接标志。
- 同时，会为 `adb_comp.o` 对象文件添加 `-DHAVE_ZSTD` 的预处理器宏定义。
- 最后，`io_zstd.o` 源文件会被加入到 `libapk.so` 动态库的构建对象列表中。

**Section sources**
- [src/Makefile](file://src/Makefile#L37-L42)

## 库文件构建规则
Makefile定义了动态库和静态库的构建规则。

### 动态库 (libapk.so)
动态库的构建规则由 `shlibs-y` 变量触发。`libapk.so.$(libapk_soname)-objs` 变量列出了构成 `libapk.so` 的所有对象文件。构建过程会根据 `URL_BACKEND` 和 `ZSTD` 的配置，动态地将 `io_url_$(URL_BACKEND).o` 和 `io_zstd.o`（如果启用）加入到对象文件列表中。链接时，会使用 `LDFLAGS_libapk.so.$(libapk_soname)` 中的 `-Wl,-soname` 参数来设置共享库的内部名称。

**Section sources**
- [src/Makefile](file://src/Makefile#L20-L49)

### 静态库 (libapk.a)
静态库 `libapk.a` 的构建规则由 `libs-y` 变量触发。其对象文件列表 `libapk.a-objs` 直接继承自动态库的对象文件列表 `$(libapk.so.$(libapk_soname)-objs)`，确保了功能的一致性。构建过程使用 `ar` 命令将所有对象文件打包成一个静态归档文件。

**Section sources**
- [src/Makefile](file://src/Makefile#L51-L59)

## 可执行文件链接过程
`apk` 可执行文件的构建由 `progs-y` 变量控制。`apk-objs` 变量列出了所有需要编译并链接到最终可执行文件中的对象文件。链接过程通过 `LIBS_apk.so` 变量指定 `-L$(obj) -lapk`，指示链接器在输出目录中查找并链接 `libapk.so` 动态库。此外，还定义了 `apk.static` 目标，用于生成静态链接的 `apk` 可执行文件，其链接库 `apk-static-libs` 包含了 `libapk.a` 静态库。

**Section sources**
- [src/Makefile](file://src/Makefile#L63-L82)

## 编译链接参数设置
构建系统通过一系列变量来设置编译和链接参数。
- **`CFLAGS_ALL`**: 这是核心的编译标志变量。它包含了通用的C语言标准、警告选项（`-Wall`）、GNU扩展（`-D_GNU_SOURCE`）以及项目特定的头文件路径（`-Iportability -Isrc`）。此外，它还会动态地将 `CRYPTO_CFLAGS`、`ZLIB_CFLAGS` 和 `ZSTD_CFLAGS` 添加进来，以确保所有依赖库的编译要求都被满足。
- **`LIBS`**: 这是核心的链接库变量。它使用 `--as-needed` 链接器标志来优化最终的可执行文件大小，并将 `CRYPTO_LIBS`、`ZLIB_LIBS` 和 `ZSTD_LIBS` 中查询到的库链接进来。

**Section sources**
- [src/Makefile](file://src/Makefile#L74-L88)

## 基本使用示例
要使用传统Makefile构建系统，可以在项目根目录下执行 `make` 命令。可以通过环境变量或命令行参数来覆盖默认配置。例如：
- `make`：使用默认配置（`URL_BACKEND=libfetch`, `CRYPTO=openssl`）进行构建。
- `make URL_BACKEND=wget CRYPTO=mbedtls`：使用wget作为URL后端，并使用mbedTLS作为加密后端进行构建。
- `make static`：构建静态链接的 `apk` 可执行文件。

**Section sources**
- [Makefile](file://Makefile#L54-L55)

## 局限性与过渡建议
传统Makefile构建系统的主要局限性在于其**平台依赖性**，仅限于musl-linux环境。这极大地限制了项目的可移植性。鉴于其已被标记为弃用，强烈建议新用户和现有用户**优先使用Meson构建系统**。Meson系统通过 `meson_options.txt` 文件提供了更现代、更灵活的配置选项（如 `url_backend`, `crypto_backend`, `zstd`），并且具有跨平台兼容性，是apk-tools项目当前和未来的官方构建方式。

**Section sources**
- [README.md](file://README.md#L8-L28)
- [meson_options.txt](file://meson_options.txt#L1-L15)