# 内嵌剪贴板首版：构建、使用与协议

更新：2026-10-15。基线：Ubuntu `ea9190bcd`，在 `macos` 分支合并并统一实现。

这是直接内嵌 Deskflow core/GUI 的实验首版，不需要独立 SSH 工具或手动启动服务。
当前完成代码集成与 Mac 编译；**尚无本次新增链路的双机运行验收结果**。
遵照用户要求，本轮不运行单元、桌面、挂载或网络测试；历史原型结果不能替代此次验收。

## 范围

- 一台 macOS 与一台 Ubuntu **X11 登录会话**，任一方可作 Deskflow server。
- 文件、目录、多文件、空文件、UTF-8 名称；远端公告只有固定元数据，FUSE 在应用读取时请求正文。
- 普通文件显式使用 `detectable`，检测到变更返回错误；不承诺不可变快照。
- 文本、HTML、PNG 使用同一调度与连接。首版对这些小格式先做有界采集（总计 16 MiB），
  再以最多 256 KiB 的逐块确认传输；此处不是远程格式正文的按需 RPC。
- 旧版对端使用原有剪贴板；启用新通道时暂停旧剪贴板采集/写入，键鼠仍走原主连接。
- 多客户端中转、Wayland Portal/XWayland 全桌面桥接、Windows、File Provider、拖放、剪切移动不在本版范围。
  第二个客户端加入会关闭已建立的文件通道；回到单客户端后需重新连接。

## 构建

两端必须包含同一集成提交。**Ubuntu 不能仅运行 `ea9190bcd`**：那个提交只提供本地组件和探针，
没有协议 1.9 代理、TLS 绑定或 GUI 开关。Ubuntu 组应合并本次 `macos` 集成分支后构建，勿再独立定义线格式。

```sh
cmake -S . -B build/clipboard-v1 -G Ninja \
  -DBUILD_CLIPBOARD_FILES=ON -DBUILD_TESTS=OFF -DBUILD_TESTING=OFF
cmake --build build/clipboard-v1 --parallel 4
```

Mac 另需现有 Qt/OpenSSL 构建参数与 macFUSE FUSE3 SDK；本机编译产物在
`build/macos-core/bin/Deskflow`、`deskflow-core`、`deskflow-clipboard-probe`、`deskflow-file-prototype`。
Ubuntu 需原工程依赖及 FUSE3、X11、XFixes 开发包。功能默认不编译、不启用；未编译时 GUI 开关不可选。
CMake 会将两个内部 helper 安装到 core 同一目录；Mac bundle 构建也会把它们放入 `Contents/MacOS`。
helper 仍使用原型名称以复用已有实现，但由 core 自动管理，不是用户需要启动的独立产品。
本轮未生成、签名或公证可分发安装包。

## 使用

1. Mac 安装 macFUSE 并启用其 FSKit 模块；Ubuntu 使用 X11 登录会话与可用的 FUSE3。
2. 两端使用本版 Deskflow，保留“共享剪贴板”总开关；启用 TLS，server 要求并验证客户端证书。
   按原 Deskflow 流程信任双方指纹。未满足双向认证时，文件扩展不可用，主连接仍可正常工作。
3. 两端设置中勾选 **Share files with macOS / Ubuntu (experimental)**，重启连接。
   对应持久配置为 `core/fileClipboard=true`。设置变更以重新连接生效。
4. 在 GUI 状态栏查看 `Clipboard: Ready…`，在文件管理器复制文件，到另一端正常粘贴。
   切屏不是传输触发条件。保持 Deskflow 连接和源文件不变直到应用复制完成。
5. 停止或断开会撤销租约、终止读取、关闭挂载。重连不会继续旧复制，需要重新复制源文件。

文件连接使用 server 动态分配的 TCP 端口，通过已认证主连接公告；防火墙需允许 Deskflow core 的此类入站连接。
仅放行原键鼠端口的环境可能出现“键鼠可用、文件通道超时”。

## 公共代码唯一入口

| 文件 | 用途 |
| --- | --- |
| `src/lib/clipboard/FileTypes.h` | 状态、OfferInfo、FileInfo、租约、读响应 |
| `LocalFileSource.h` | 真实文件源、固定树、版本检查与 whole-offer 租约 |
| `LocalFileRpc.h` | 共用帧编码/解码、offer 编解码、文件请求分派 |
| `ClipboardBroker.h` | 原 Ubuntu 文件公告纯状态组件和 manifest 校验；集成运行时使用自己的单线程双机排序 |
| `CoreClipboard.h/.cpp` | core 持有的 PeerLink、专用线程、认证通道、平台 helper、挂载生命周期 |
| `src/apps/clipboard-prototype/RpcFileSource.*` | 租约先于发布、空闲续租、短读聚合、FUSE 错误映射 |

旧 Mac `FileService`/`LocalFileSource::capture()` 和 JSONL 服务迁至
`clipboard/experimental/` 命名空间 `deskflow::clipboard::experimental`，仅作历史实验。
生产侧不要包含 experimental 类型或把 JSONL 当网络协议。

## 本 fork 的线协议 v1

以下是本 fork 的固定 profile，不是上游 Deskflow 正式分配的协议版本。
任何破坏兼容性的字段/顺序/预算变更都必须升级 schema，不能在相同 v1 下各端自行扩展。

### 协商与认证

主协议 minor 为 **1.9**；协商为 1.8 或更低时绝不发送新消息。
server 在接纳配置名称、排除重复名称、发送 `DSOP` 后发送：

```text
DCAP | schema:u16BE (=1) | port:u16BE (0=不可用)
```

client 仅在本机启用、平台可用且主连接已认证时主动连接；不发送额外主流探测消息。
未连接/绑定在 15 秒内完成则文件扩展失败，不关闭键鼠连接。

独立连接使用 TLS >=1.2，本地证书沿用主连接配置；对端证书 SHA-256 必须等于本主连接已验证的证书。
主连接只有双方认证完成才导出 32 字节密钥：
`SSL_export_keying_material(label="EXPORTER-Deskflow-Clipboard-v1", context absent)`。
绑定值仅在专用 TLS 内传输，不进日志、IPC、命令行；双方先发 `bind,1,hex(exporter)`，验证后互发 `ready`。
双方 `ready` 完成才启动系统监听与文件 RPC。监听器在成功绑定后关闭；同一主连接只允许一次绑定。
错误的未认证连接不会消费绑定，仍受原 15 秒期限限制。

epoch 为 `SHA256(exporter || "server")` 或 `SHA256(exporter || "client")` 的十六进制值，来源方不同。
主 proxy 销毁即撤销会话；新主 TLS 握手产生新绑定与 epoch，不复用旧句柄。

### 帧

所有消息共用 Ubuntu 本地 RPC 的编码器：

```text
u32BE bodyLength (不含本字段，8..1048576)
  u32BE fieldCount (<=16384)
  重复 fieldCount 次: u32BE byteLength (<=65536), UTF-8 字节
  u32BE payloadLength
  payload 原始字节
```

`uint64`/ID/offset 在 fields 中使用十进制字符串，不经过 JSON 浮点数。
只有 `format-chunk` 和文件 read 成功响应携带正文。残帧不能作为成功响应。

| fields | 语义 |
| --- | --- |
| `bind,1,hexSecret` / `ready` | 上述认证与 profile 确认，payload 必须空 |
| `intent,localToken` | client 本地剪贴板变化，token 严格递增 |
| `select,serverRevision,server或client,localToken` | server 分配全局顺序；新选择使旧的待发布结果失效 |
| `files,localToken,offerFields…` | 仅为当前选择发送 manifest，格式见下文 |
| `format-start,localToken,mime,totalBytes` | mime 只能 text/plain、text/html、image/png，总预算 16 MiB |
| `format-chunk,localToken,mime,offset` + bytes | <=256 KiB；顺序且 offset 精确匹配 |
| `format-ack,localToken,mime,nextOffset` | 初始 ACK 为 0；发端收到 ACK 后才发下一块 |
| `formats-end,localToken` | 所有表示完整接收后才允许发布 |
| `request,bridgeId,op,requestId,args…` | 文件请求，bridge 内 ID 严格递增 |
| `reply,bridgeId,op,requestId,status,result…` + bytes | 精确关联请求；read 响应交 FileRpcClient 再次校验 |

`offerFields` 顺序：epoch、offerId、offerVersion、acquisitionWindowMs、immutable(0/1)、
rootCount、rootId…、fileCount，然后每文件 7 字段：fileId、parentId、name、relativePath、version、file或directory、size。
具体 kind 字面量以 `LocalFileRpc.cpp::putOffer/getOffer` 为准（`file` / `directory`）。
manifest 在接收端校验层级、根、重复名称、路径和预算，文件名还须是有效 UTF-8。
不能把绝对路径当文件访问权限。

文件 RPC 参数（和 Ubuntu `FileRpcClient` 完全一致）：

```text
acquire: epoch, offerId, offerVersion, ttlMs
renew:   epoch, leaseId, ttlMs
release: epoch, leaseId
open:    epoch, leaseId, fileId, version, immutable或detectable
read:    epoch, leaseId, handleId, offset, length
close:   epoch, handleId
```

响应以 `op,requestId,status` 开头。acquire/renew/release 后接 leaseId、offerVersion、grantedTtlMs、leaseState；
open 后接 handleId、version、size；close 无附加字段；read 后接 epoch、offerId、fileId、version、leaseId、handleId、
requestId、offset、length、eof(0/1)，payload 为 bytes。
`hello/refresh` 只属于本机 RPC 探针，不接受远端调用；bridge 的 hello 从已接收 manifest 合成。
List/stat 由固定完整 manifest 提供，本 profile 不另设目录分页网络消息。

### 固定预算与生命周期

- manifest <=1024 entries / 512 KiB / 64 roots；源端最多 16 个保留 offer。
- 接收端最多 8 个保留挂载，各有一个串行 RPC 通道；单次读取 <=256 KiB。
- 默认租约 30 秒、最大 60 秒；RpcFileSource 包括空闲时自动续租。更新剪贴板不卸载仍有租约的旧目录。
- 应用数据发送队列 <=2 MiB + 64 KiB 信封余量，计入 Qt 明文及加密队列；超预算使通道失败，不无限缓存。
- 单格式传输为逐块 ACK，总正文 <=16 MiB；接收端最多保留正在使用与替换中的两组有界格式文件。
  文件正文不会预下载到这些临时文件。
- 本机读取/文件 RPC 为 10 秒期限，挂载启动 15 秒，格式 ACK 10 秒。
  底层文件系统系统调用可能阻塞，原生 AppKit 内部分配也不由该预算控制；不宣称硬实时资源上限。
- FUSE 取得租约、校验 manifest、完成挂载初始化后才发布本机 URL。
  新的本地复制、server revision 或 native owner/changeCount 不符会阻止过期发布。
- 正常退出依次关闭 bridge、撤销源、终止 helper、卸载仅本会话拥有的路径。
  helper 监视 core 父进程；异常退出后请求结束。无法卸载的路径保留并记录日志，不递归删除挂载树。

## 定位问题与验收边界

GUI 展示可用、失败、挂载和断线状态；Debug 日志含文件 op、requestId、status、returnedBytes、offset、length，
不记录 TLS 绑定密钥或文件正文。大量逐范围日志默认不写独立增长的 JSONL 文件。
到达挂载保留上限后，先等旧复制结束再重连；不要在应用仍读取时手动卸载。

本轮仅有：Mac feature-on 完整编译、core/GUI/helper 增量编译、X11 helper 语法编译检查。
没有实际跨机 TLS/Finder/Files 复制结果，没有新增资源峰值、取消矩阵、旧端握手或打包签名结果。
后续双机验收使用本文件冻结的协议，并复用 `clipboard-m5-integration.md` 的场景清单；不得将编译成功记为 M5 验收通过。
