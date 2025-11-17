# 网络与IO

<cite>
**本文档引用的文件**
- [io.c](file://src/io.c)
- [io_url_libfetch.c](file://src/io_url_libfetch.c)
- [io_gunzip.c](file://src/io_gunzip.c)
- [io_zstd.c](file://src/io_zstd.c)
- [apk_io.h](file://src/apk_io.h)
- [http.c](file://libfetch/http.c)
- [fetch.h](file://libfetch/fetch.h)
- [fetch.c](file://libfetch/fetch.c)
</cite>

## 目录
1. [输入输出子系统概述](#输入输出子系统概述)
2. [IO分层架构](#io分层架构)
3. [网络IO实现](#网络io实现)
4. [压缩格式支持](#压缩格式支持)
5. [性能优化策略](#性能优化策略)
6. [错误处理与重试机制](#错误处理与重试机制)
7. [数据流图](#数据流图)

## 输入输出子系统概述

apk-tools的输入输出子系统设计用于高效处理从远程仓库到本地文件系统的数据传输。该系统通过分层架构实现了网络IO、文件IO和压缩解压功能的解耦，提供了灵活且可扩展的数据处理能力。核心组件包括`apk_istream`抽象流、libfetch网络库适配层以及对zstd和gzip压缩格式的支持。

**Section sources**
- [io.c](file://src/io.c#L79-L85)
- [apk_io.h](file://src/apk_io.h#L77-L84)

## IO分层架构

apk-tools的IO子系统采用分层架构设计，从高层的`apk_istream`抽象到底层的具体实现，形成了清晰的层次结构。`apk_istream`结构体是整个IO系统的核心，它通过`ops`字段指向具体的`apk_istream_ops`操作集，实现了多态性。

```mermaid
classDiagram
class apk_istream {
+uint8_t *ptr
+uint8_t *end
+uint8_t *buf
+size_t buf_size
+int err
+unsigned int flags
+struct apk_progress *prog
+const struct apk_istream_ops *ops
}
class apk_istream_ops {
+void (*get_meta)(struct apk_istream *is, struct apk_file_meta *meta)
+ssize_t (*read)(struct apk_istream *is, void *ptr, size_t size)
+int (*close)(struct apk_istream *is)
}
class apk_fetch_istream {
+struct apk_istream is
+fetchIO *fetchIO
+struct url_stat urlstat
}
class apk_fd_istream {
+struct apk_istream is
+int fd
}
class apk_zstd_istream {
+struct apk_istream is
+struct apk_istream *input
+ZSTD_DCtx *ctx
+void *buf_in
+size_t buf_insize
+ZSTD_inBuffer inp
+int flush
}
class apk_gzip_istream {
+struct apk_istream is
+struct apk_istream *zis
+z_stream zs
+apk_multipart_cb cb
+void *cbctx
+void *cbprev
+apk_blob_t cbarg
}
apk_istream --> apk_istream_ops : "包含"
apk_fetch_istream --> apk_istream : "继承"
apk_fd_istream --> apk_istream : "继承"
apk_zstd_istream --> apk_istream : "继承"
apk_gzip_istream --> apk_istream : "继承"
```

**Diagram sources**
- [apk_io.h](file://src/apk_io.h#L77-L84)
- [io_url_libfetch.c](file://src/io_url_libfetch.c#L21-L25)
- [io.c](file://src/io.c#L547-L550)
- [io_zstd.c](file://src/io_zstd.c#L20-L27)
- [io_gunzip.c](file://src/io_gunzip.c#L18-L27)

**Section sources**
- [apk_io.h](file://src/apk_io.h#L77-L84)
- [io_url_libfetch.c](file://src/io_url_libfetch.c#L21-L25)
- [io.c](file://src/io.c#L547-L550)
- [io_zstd.c](file://src/io_zstd.c#L20-L27)
- [io_gunzip.c](file://src/io_gunzip.c#L18-L27)

## 网络IO实现

### libfetch库适配层

apk-tools通过`io_url_libfetch.c`文件中的适配层实现了对libfetch库的封装。`apk_io_url_istream`函数是网络IO的主要入口，它将libfetch的`fetchIO`对象包装成`apk_istream`接口。

```mermaid
sequenceDiagram
participant Client as "客户端"
participant apk_io as "apk_io_url_istream"
participant libfetch as "libfetch"
participant Server as "远程服务器"
Client->>apk_io : 请求URL
apk_io->>libfetch : fetchParseURL()
libfetch-->>apk_io : URL结构体
apk_io->>libfetch : fetchXGet()
libfetch->>Server : HTTP请求
Server-->>libfetch : 响应
libfetch-->>apk_io : fetchIO对象
apk_io->>Client : apk_istream对象
loop 数据读取
Client->>apk_io : apk_istream_read()
apk_io->>libfetch : fetchIO_read()
libfetch-->>apk_io : 数据
apk_io-->>Client : 数据
end
Client->>apk_io : apk_istream_close()
apk_io->>libfetch : fetchIO_close()
```

**Diagram sources**
- [io_url_libfetch.c](file://src/io_url_libfetch.c#L130-L174)
- [fetch.c](file://libfetch/fetch.c#L57-L71)
- [http.c](file://libfetch/http.c#L810-L1183)

**Section sources**
- [io_url_libfetch.c](file://src/io_url_libfetch.c#L130-L174)
- [fetch.c](file://libfetch/fetch.c#L57-L71)
- [http.c](file://libfetch/http.c#L810-L1183)

### HTTP/HTTPS网络IO

网络IO的实现基于libfetch库，支持HTTP和HTTPS协议。`http_request`函数处理完整的HTTP请求流程，包括连接建立、请求发送、响应处理和重定向。

```mermaid
flowchart TD
A[开始] --> B[解析URL]
B --> C[检查代理设置]
C --> D[建立连接]
D --> E[发送HTTP请求]
E --> F[读取响应]
F --> G{响应码}
G --> |200/206| H[处理数据]
G --> |301/302/307| I[处理重定向]
G --> |401| J[处理认证]
G --> |其他| K[错误处理]
H --> L[返回apk_istream]
I --> M[更新URL]
M --> D
J --> N[获取认证信息]
N --> E
K --> O[返回错误]
```

**Diagram sources**
- [http.c](file://libfetch/http.c#L810-L1183)
- [fetch.c](file://libfetch/fetch.c#L57-L71)

**Section sources**
- [http.c](file://libfetch/http.c#L810-L1183)
- [fetch.c](file://libfetch/fetch.c#L57-L71)

## 压缩格式支持

### zstd压缩支持

apk-tools通过`io_zstd.c`文件实现了对zstd压缩格式的支持。`apk_istream_zstd`函数创建一个zstd解压流，将压缩数据流转换为原始数据流。

```mermaid
classDiagram
class apk_zstd_istream {
+struct apk_istream is
+struct apk_istream *input
+ZSTD_DCtx *ctx
+void *buf_in
+size_t buf_insize
+ZSTD_inBuffer inp
+int flush
}
class zstd_istream_ops {
+get_meta
+read
+close
}
apk_zstd_istream --> zstd_istream_ops : "使用"
apk_zstd_istream --> apk_istream : "继承"
```

**Diagram sources**
- [io_zstd.c](file://src/io_zstd.c#L20-L27)
- [io_zstd.c](file://src/io_zstd.c#L95-L99)

**Section sources**
- [io_zstd.c](file://src/io_zstd.c#L20-L27)
- [io_zstd.c](file://src/io_zstd.c#L95-L99)

### gzip压缩支持

通过`io_gunzip.c`文件实现了对gzip压缩格式的支持。`apk_istream_zlib`函数创建一个zlib解压流，支持gzip和deflate格式。

```mermaid
classDiagram
class apk_gzip_istream {
+struct apk_istream is
+struct apk_istream *zis
+z_stream zs
+apk_multipart_cb cb
+void *cbctx
+void *cbprev
+apk_blob_t cbarg
}
class gunzip_istream_ops {
+get_meta
+read
+close
}
apk_gzip_istream --> gunzip_istream_ops : "使用"
apk_gzip_istream --> apk_istream : "继承"
```

**Diagram sources**
- [io_gunzip.c](file://src/io_gunzip.c#L18-L27)
- [io_gunzip.c](file://src/io_gunzip.c#L143-L147)

**Section sources**
- [io_gunzip.c](file://src/io_gunzip.c#L18-L27)
- [io_gunzip.c](file://src/io_gunzip.c#L143-L147)

## 性能优化策略

### 并行下载

apk-tools通过连接缓存机制优化网络性能。`fetchConnectionCacheInit`函数初始化连接缓存，允许重用TCP连接。

```mermaid
flowchart TD
A[开始下载] --> B{连接缓存中存在?}
B --> |是| C[重用现有连接]
B --> |否| D[建立新连接]
D --> E[添加到连接缓存]
C --> F[发送请求]
E --> F
F --> G[接收响应]
G --> H[保持连接]
H --> I[等待后续请求]
I --> B
```

**Diagram sources**
- [io_url_libfetch.c](file://src/io_url_libfetch.c#L216)
- [common.c](file://libfetch/common.c#L298-L312)

**Section sources**
- [io_url_libfetch.c](file://src/io_url_libfetch.c#L216)
- [common.c](file://libfetch/common.c#L298-L312)

### 缓存预取

系统通过`apk_io_bufsize`全局变量控制缓冲区大小，优化IO性能。默认缓冲区大小为128KB。

```mermaid
flowchart TD
A[数据读取请求] --> B{缓冲区中有数据?}
B --> |是| C[从缓冲区读取]
B --> |否| D[从源读取数据块]
D --> E[填充缓冲区]
E --> F[从缓冲区读取]
C --> G[返回数据]
F --> G
```

**Diagram sources**
- [io.c](file://src/io.c#L33)
- [io.c](file://src/io.c#L97-L104)

**Section sources**
- [io.c](file://src/io.c#L33)
- [io.c](file://src/io.c#L97-L104)

## 错误处理与重试机制

### 网络错误处理

系统通过`fetch_maperror`函数将libfetch的错误码映射到apk-tools的错误码，实现了统一的错误处理。

```mermaid
flowchart TD
A[网络错误] --> B[获取错误类别]
B --> C{错误类别}
C --> |FETCH| D[映射到EIO]
C --> |URL| E[映射到APKE_URL_FORMAT]
C --> |ERRNO| F[直接使用errno]
C --> |NETDB| G[映射到DNS错误]
C --> |HTTP| H[映射到HTTP错误]
C --> |TLS| I[映射到TLS错误]
D --> J[返回错误码]
E --> J
F --> J
G --> J
H --> J
I --> J
```

**Diagram sources**
- [io_url_libfetch.c](file://src/io_url_libfetch.c#L32-L91)

**Section sources**
- [io_url_libfetch.c](file://src/io_url_libfetch.c#L32-L91)

### 重试逻辑

系统支持条件性重试，当服务器返回304状态码（未修改）时，会返回`APKE_FILE_UNCHANGED`错误。

```mermaid
flowchart TD
A[发送请求] --> B[检查If-Modified-Since]
B --> C{已设置?}
C --> |是| D[包含If-Modified-Since头]
C --> |否| E[普通请求]
D --> F[发送请求]
E --> F
F --> G[接收响应]
G --> H{状态码}
H --> |304| I[返回未修改]
H --> |200| J[返回数据]
H --> |其他| K[返回错误]
```

**Diagram sources**
- [io_url_libfetch.c](file://src/io_url_libfetch.c#L149-L152)
- [http.c](file://libfetch/http.c#L793-L797)

**Section sources**
- [io_url_libfetch.c](file://src/io_url_libfetch.c#L149-L152)
- [http.c](file://libfetch/http.c#L793-L797)

### 代理配置

系统通过环境变量支持代理配置，自动检测HTTP_PROXY和HTTPS_PROXY环境变量。

```mermaid
flowchart TD
A[解析URL] --> B[检查代理设置]
B --> C{direct标志?}
C --> |是| D[直接连接]
C --> |否| E[检查no_proxy]
E --> F{匹配no_proxy?}
F --> |是| D
F --> |否| G[检查HTTPS]
G --> H{HTTPS?}
H --> |是| I[获取HTTPS_PROXY]
H --> |否| J[获取HTTP_PROXY]
I --> K[解析代理URL]
J --> K
K --> L[建立代理连接]
D --> M[建立直接连接]
L --> N[发送请求]
M --> N
```

**Diagram sources**
- [http.c](file://libfetch/http.c#L771-L782)
- [http.c](file://libfetch/http.c#L682-L741)

**Section sources**
- [http.c](file://libfetch/http.c#L771-L782)
- [http.c](file://libfetch/http.c#L682-L741)

## 数据流图

```mermaid
flowchart LR
A[远程仓库] --> B[HTTP/HTTPS]
B --> C[libfetch]
C --> D[apk_io_url_istream]
D --> E{压缩格式}
E --> |zstd| F[apk_istream_zstd]
E --> |gzip| G[apk_istream_gunzip]
E --> |无压缩| H[直接传输]
F --> I[解压数据]
G --> I
H --> I
I --> J[apk_istream]
J --> K[数据处理]
K --> L[本地文件系统]
```

**Diagram sources**
- [io_url_libfetch.c](file://src/io_url_libfetch.c#L130-L174)
- [io_zstd.c](file://src/io_zstd.c#L100-L133)
- [io_gunzip.c](file://src/io_gunzip.c#L154-L180)
- [io.c](file://src/io.c#L606-L610)

**Section sources**
- [io_url_libfetch.c](file://src/io_url_libfetch.c#L130-L174)
- [io_zstd.c](file://src/io_zstd.c#L100-L133)
- [io_gunzip.c](file://src/io_gunzip.c#L154-L180)
- [io.c](file://src/io.c#L606-L610)