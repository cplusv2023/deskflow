# Deskflow 剪贴板 M5 接入计划

更新日期：2026-10-15，Asia/Shanghai。

本文件保留 Ubuntu `ea9190bcd` 的原始接入计划与验收门槛。**本轮已新增双机首版 core/GUI 实现并完成 Mac 编译，尚未完成 M5 双机验收**。
现行协议和实现范围见 [内嵌首版指南](clipboard-core-v1.md)；下文“尚未实现”及 1.8 现状描述均属于合并前基线。
M0 至 M2 的同机 FUSE／Files 结果不能计作现有 core 已接入新服务、旧端兼容或 Mac 实机互通通过。M3 与 M4 的组件工作、实际通过记录以 [Ubuntu 进度](clipboard-progress.md) 为准。

目标保持为 [按需剪贴板架构](clipboard-design.md) 与 [跨端契约](clipboard-contract.md) 中的消费者读取时取数据，不使用完整文件预下载作为发布前提。当前 Ubuntu 开发假定 Mac 按接口可用；Finder 与真实 Mac 联调另外验收。

## 先完成的基础与接口边界

M5 依赖以下基础，不能用开关或空处理函数代替：

- Ubuntu 原生应用的文本、HTML、PNG、文件列表采集与发布，以及独立于键鼠焦点的变化监听。
- 真实本地文件的受公告约束访问，64 位范围、显式 EOF、错误、版本、租约和有界预算。
- 两进程请求关联、故障、取消、旧目录引用保留以及来源回环抑制。

[ClipboardBroker](../../src/lib/clipboard/ClipboardBroker.h) 是 M4 的纯 C++20 状态组件：一条已认证 peer 会话、动作列表、先取得租约再发布、延迟完成防覆盖、旧目录保留及预算。它没有调用 X11、FUSE、网络、GUI 或现有 core，也不建立或认证连接。其 `revision` 是本会话来源的单调序列；`selectionRevision` 是本 Broker 的接收顺序。未来多端服务端仍需分配全局 wire revision，不能把这两个局部计数当作多机全局顺序。

当前 Broker 的输入是文件 OfferInfo；文本／HTML／PNG 的条目和多 MIME 表示仍需扩展到统一公告模型，再接入持续运行的后端。独立四格式探针通过不代表 Broker 已路由所有格式。

Broker 的全部调用和动作应用必须在同一调度器串行执行。异步平台发布在最终 claim 前须在该调度器检查 `canPublish(token)`，同步完成 claim 再回报 `completePublication`；过期动作不能在另一线程绕过检查写入系统剪贴板。`ownerId` 必须来自后端实际拥有者，不能仅信任正文中携带的标记。动作保留不可变 manifest 的共享引用；动作执行后调用方需释放动作列表。

## Core 生命周期入口

新增一个 core 内的 `ClipboardRuntime`，负责本地监听、平台提供器、FUSE 生命周期、租约时钟及调度；每条被正式接纳的连接创建 `ClipboardPeerSession`。这些名称是计划中的服务，尚未接入 core。

| 现有入口 | 接入职责 |
| --- | --- |
| [ClientApp.cpp](../../src/lib/deskflow/ClientApp.cpp)，`startClient()` 约 273 行、`stopClient()` 约 310 行 | 启停 runtime；监听与连接状态分开；退出时不遗留线程或挂载。 |
| [ServerProxy.cpp](../../src/lib/client/ServerProxy.cpp)，`parseHandshakeMessage()` 约 138 行 | 收到 `DSOP` 后才调用 `handshakeComplete()`；TCP/TLS connected 不能视为业务会话就绪。 |
| [Client.cpp](../../src/lib/client/Client.cpp)，`cleanup()` 约 508 行、`cleanupComputer()` 约 540 行 | 删除 proxy／stream 前撤销 peer epoch、结束未决读取和续租、关闭其文件连接。 |
| [Server.cpp](../../src/lib/server/Server.cpp)，`adoptClient()` 约 245 行 | 经过配置名称、重复名称检查并发送 options 后，才为 peer 建立扩展会话。 |
| [Server.cpp](../../src/lib/server/Server.cpp)，`removeClient()` 约 1923 行 | 释放这个 peer 的会话、请求与租约；不误撤销其他 peer 仍有效的本地源引用。 |
| [ServerApp.cpp](../../src/lib/deskflow/ServerApp.cpp)，`stopServer()` 约 197 行、`cleanupServer()` 约 230 行 | 停止发布，取消等待，关闭数据连接，退出 FUSE，再销毁依赖对象；reset／reload／异常启动失败走同一清理规则。 |

重连使用新 `sessionEpoch`。本地桌面 runtime 可跨一次网络重连存在，但旧 peer 句柄、租约、请求、绑定凭据不得迁移到新 epoch。FUSE 线程可等待有期限的异步结果；键鼠事件循环与 SocketMultiplexer 线程不得同步等待文件读取或执行重型文件处理。

## 扩展协议协商

当前协议上限为 1.8，具体证据：

- [ProtocolTypes.h](../../src/lib/deskflow/ProtocolTypes.h) 约 48 行定义 minor=8。
- [Client.cpp](../../src/lib/client/Client.cpp) 约 696 行按双方上限的较小值回应；`setupComputer()` 约 467 行只接受 6、7、8。
- [ClientProxyUnknown.cpp](../../src/lib/server/ClientProxyUnknown.cpp) 的 `initProxy()` 约 140 行只选择至 `ClientProxy1_8`。
- [ServerProxy.cpp](../../src/lib/client/ServerProxy.cpp) 约 100 行与 [ClientProxy1_0.cpp](../../src/lib/server/ClientProxy1_0.cpp) 约 124 行遇到未知消息会丢弃剩余流数据，无法安全跳过未知负载。

实施时需为本分支明确分配扩展 minor，同时增加双方代理和相应选择分支。示例 `1.9` 只是候选，尚未成为已实现或上游正式分配的版本。两端协商结果落在本分支扩展版本后，才能发送 `deskflow.clipboard.v1` 能力消息；1.8 连接上不能发送能力探测或文件消息。

扩展状态建议为：

```text
Legacy → ExtensionNegotiating → Enabled
                            ↘ Unavailable
```

`Enabled` 要求双方明确确认具体能力、schema、大小、并发、租约与一致性模式。能力超时或不支持时保留键鼠连接并报告扩展不可用，不能让普通连接永久等能力响应。

优先在原握手完成后协商。若消息可能在握手阶段到达，新代理的 `parseHandshakeMessage()` 与 `parseMessage()` 都必须处理，避免被基类丢弃。控制消息使用有界、含 schema／类型／负载长度的封装；文件字节通过独立连接传输。

服务端负责新端之间公告的全局顺序与中转路由。请求按来源 peer、epoch、offer、file、lease 和 handle 路由，不依赖当前活动屏幕，不把不同来源的局部 fileId 当成全局唯一值。

## Ubuntu 后端与 legacy 统一发布

当前键鼠后端同时决定剪贴板路径：

- [ClientApp.cpp](../../src/lib/deskflow/ClientApp.cpp) 约 116 行与 [ServerApp.cpp](../../src/lib/deskflow/ServerApp.cpp) 约 401 行在 Wayland 上直接建立 `EiComputer`。
- [EiComputer.cpp](../../src/lib/platform/EiComputer.cpp) 约 57 行创建 Input Capture／Remote Desktop Portal；约 178、482 行直接读写 Portal 缓存。
- [PortalInputCapture.cpp](../../src/lib/platform/PortalInputCapture.cpp) 约 742 行在激活时读剪贴板并 claim 所有权。
- [PortalRemoteDesktop.cpp](../../src/lib/platform/PortalRemoteDesktop.cpp) 约 230、256 行直接发布和处理变化。

因此，新增 X11 后端时必须选择系统剪贴板的唯一负责人，并抑制原 Portal 的读取、claim 与变化事件。只让新服务统一 publish 不够：切屏仍可能触发原 Input Capture 路径覆盖新内容。键鼠仍保留 Portal，不改变其授权与捕获路径。

legacy 适配入口可集中在 [Computer.cpp](../../src/lib/deskflow/Computer.cpp) 的 `setClipboard()` 约 207 行与 `getClipboard()` 约 397 行。客户端 `Client::setClipboard()`／`ServerProxy::setClipboard()`、服务端 `Server::onClipboardChanged()` 将旧消息转换为 Broker 事件，由统一发布器写系统剪贴板。

新端按 peer 能力分发公告；旧端继续使用原 Text／HTML／Bitmap 和 `CCLP/DCLP`。不向旧端发送源机器绝对路径或本机 FUSE 挂载 URI 作为文件退化表示。现有 [Server.cpp](../../src/lib/server/Server.cpp) 约 1478 行只向 `m_active` 发内容，[Client.cpp](../../src/lib/client/Client.cpp) 约 660 行也会按 active 延迟发送，这些触发规则不能直接用于新端持续共享。

## 独立文件 TLS 连接

可复用现有 `TCPSocketFactory`、`SecureListenSocket`、`SecureSocket`，但建立专用文件 listener／session，不进入键鼠 `ClientProxyUnknown`。

```text
主连接认证、配置接纳和扩展能力确认
 → 主连接下发短期、单次绑定凭据
 → 客户端主动建立文件 TLS 连接
 → 验证主会话、epoch、对端身份和绑定凭据
 → 接受范围读取与文件数据
```

凭据绑定主会话 ID、epoch、已接纳 peer、用途、期限和单次使用状态。只匹配 IP、名称，或只检查证书在全局信任库中，都不足以证明这是对应的主会话。凭据不进入日志；重连后旧凭据失效。仅文件连接断开时，可按契约在主会话和租约仍有效的前提下以新 requestId 重试固定版本。

现有 TLS 不是所有配置都双向认证：

- [ServerApp.cpp](../../src/lib/deskflow/ServerApp.cpp) 约 443 行区分 `PlainText`、`Encrypted`、`PeerAuth`。
- [SecureSocket.cpp](../../src/lib/net/SecureSocket.cpp) 约 433 行仅在 `PeerAuth` 下验证客户端指纹；约 496 行验证服务端指纹。
- [SecureSocket.h](../../src/lib/net/SecureSocket.h) 目前只有 readiness 等公开接口，未公开已验证 peer 身份。

首版可把文件扩展限定在满足认证要求的主会话，否则明确报告文件能力不可用。实现时新增只读的已验证 peer 身份元数据，数据连接校验与该主会话相同的身份，不能解析日志或 GUI IPC 字符串取得认证结论。

独立连接仍需有界排队、发送窗口和读写调度。共用 SocketMultiplexer 时，单个文件处理回调仍可能影响所有连接，必须测量，不能把“不同 TCP 连接”直接等同于“键鼠响应不受影响”。

## GUI 与配置

现有剪贴板配置在 [ServerConfigDialog.cpp](../../src/lib/gui/dialogs/ServerConfigDialog.cpp)：约 76 行保存开关并重置 Portal token，约 234、249 行处理开关与大小。[Config.cpp](../../src/lib/server/Config.cpp) 约 555 行转换为旧协议 options；GUI 大小使用 MiB，旧选项使用 KiB。

保留总共享开关，新增文件能力与状态时明确区分格式数据上限、文件大小、在途字节、请求数、租约数及元数据预算。不能把旧的 3 MiB 格式上限误用于大文件，也不能把无限制当成实现有界内存的方法。

用 [CoreIpc.h](../../src/lib/deskflow/ipc/CoreIpc.h) 的现有 IPC 报告后端、能力、传输进度、错误与不可用原因。GUI 不搬运正文，不以网络请求完成或租约释放代替原生应用复制完成。

## 集成切片与验收门槛

| 切片 | 完成条件 | 当前状态 |
| --- | --- | --- |
| M5-A runtime 与统一后端 | 实际 core 持有生命周期；Portal／X11 不抢所有权；退出、失败启动、reset 不泄漏资源。 | 未实现 |
| M5-B 协商与协议 | 新端确认能力后才启用；新旧双方向握手和旧消息格式不回归；不向 1.8 发扩展。 | 未实现 |
| M5-C 数据连接 | 真正 TLS 与主会话身份绑定；范围请求、取消、错误、背压和重连通过。 | 未实现 |
| M5-D Broker／legacy／多端路由 | 焦点独立，单一系统发布；旧目录在替换后可继续访问；新旧混合 peer 不回环。 | 未实现；Broker 组件不等于完成 core 接入 |
| M5-E 设置与回归 | 关闭共享及时停止新发布和请求；状态准确；原有测试、交互和资源清理通过。 | 未实现 |

以下必须取得实际证据，不能由现有同机生成源测试推定：

1. 新客户端连接旧服务端、旧客户端连接新服务端、新旧客户端混合，以及 1.8 报文中没有扩展消息。
2. 实际 GNOME core 下切屏、返回、焦点不变、剪贴板替换和再次复制相同内容；验证原 Portal 未抢占，且合法复制未被内容哈希去重吞掉。
3. 两进程 TLS 链路；错误身份、错误 epoch、过期／重放凭据拒绝，不复用旧句柄或残缺帧。
4. 主连接／文件连接分别断开，挂起、恢复、core stop/reset/异常退出；阻塞读取及时结束，无残留进程、挂载、请求或续租。
5. 目录 A 复制时换成 B，在 A 所有已打开句柄关闭后仍能打开其尚未访问的子项；回收只由明确释放或失效边界触发。
6. 大文件、延迟、并发、短响应、断线和取消期间的实际键鼠响应、峰值内存、在途字节与临时落盘；不同连接本身不构成通过证据。
7. 真实源文件变更与一致性模式；不可变生成源不能证明普通文件满足 immutableVersion。
8. Mac 实机与 Finder 联调。Ubuntu 与模拟端通过只记录为 Ubuntu 侧结果。

若只完成其中一个组件或本机测试，进度应记录该切片已完成的部分与剩余门槛，不能将 M5 总体标为完成。
