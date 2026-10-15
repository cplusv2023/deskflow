# Deskflow 按需剪贴板构建目标与架构

更新日期：2026-10-15，Asia/Shanghai。

目标是在 macOS 与 Ubuntu 之间提供接近 Windows RDP 的双向复制粘贴体验：先公布剪贴板内容及格式，目标应用实际读取时再传输数据。文件在公布到对端剪贴板前不完整预下载，也不要求用户先等待 Deskflow 准备好完整缓存文件。

本阶段由当前 Ubuntu 机器负责平台原型测试，**假定 Mac 端能够按约定接口提供和接收数据**。Mac 平台开发、Finder 行为与安装部署不作为当前阶段的前置任务；该假设不等于 Mac 与 Ubuntu 已经联调通过。

实际环境、实现状态、验收用例和后续记录见 [Ubuntu 原型进度](clipboard-progress.md)。M0 至 M2 已验证；M3/M4 新增四格式 X11 后端、LocalFileSource、Broker 状态组件和同用户 RPC/FUSE 链路。它们尚未组成统一运行时，正式协议与 core 会话整合仍为待实现设计，见 [M5 接入计划](clipboard-m5-integration.md)。

两端需要确认的读取结果、租约、版本错误和共享样本见 [跨端接口契约 v1 草案](clipboard-contract.md)。

## 用户体验目标

| 场景 | 目标行为 |
| --- | --- |
| 文本 | 任一端复制后，另一端可直接在原生应用中粘贴，不依赖鼠标切屏。 |
| HTML 与图片 | 一次复制保留多种可用表示，由目标应用选择格式；跨系统的应用私有格式不承诺完整保真。 |
| 文件与目录 | 对端先呈现可粘贴的文件入口，文件管理器读取时再请求内容，使用正常的复制流程。 |
| 大文件 | 支持 64 位文件大小和偏移；内存占用受并发窗口和缓冲预算限制，不随文件总大小线性增长。 |
| 再次复制 | 新剪贴板替换当前公告，已经打开的旧文件传输仍引用原来的内容版本。 |
| 失败与取消 | 断线、取消、权限变化和源文件变化有明确结果，不把不完整数据报告为成功。 |
| 键鼠响应 | 文件读取、磁盘操作和网络等待不阻塞键鼠事件处理。 |

“按需”以消费者的读取为触发条件。预览、缩略图和索引器也可能读取文件，因此不承诺只有按下 Ctrl+V 才产生数据传输，也不通过拦截粘贴快捷键判断传输时机。

允许有界的内存块缓冲、有限预读以及操作系统页缓存。排除的是应用层完整文件预下载和完整中间副本，而不是所有形式的缓存。目标目录中的最终文件正常落盘。

当前阶段不包含跨端剪切与删除、拖拽、剪贴板历史库、离线文件保存、任意应用私有对象格式，以及 Windows 客户端实现。不引入实际 RDP 连接，也不以兼容 MS-RDPECLIP 线格式为目标。

## Ubuntu 原型范围

首个验收环境是当前 Ubuntu GNOME Wayland 桌面与 Files 文件管理器。原型覆盖两个方向：

- 接收方向：模拟 Mac 公告远端文件，Ubuntu 将其发布到剪贴板，Files 粘贴时按范围读取。
- 发送方向：Ubuntu 原生应用复制内容，Ubuntu 后端提取格式和文件清单，并响应模拟对端的数据读取。

先使用同机的模拟源或独立进程代替 Mac。模拟端使用与正式协议一致的文件标识、64 位偏移、读取长度和错误语义，支持人为制造延迟、断线和文件变化。同机测试证明 Ubuntu 路径与接口契约，不计作真实 Mac 联调。

验证顺序是远端文件读取入口、原生剪贴板接入、模块整合、正式连接接入。文本同步不能替代文件按需读取的可行性验证。

## 现有 Deskflow 的边界

当前源码基线是 `c46781f19b1f5e39df67266665084de15f91b01c`。已有实现可以继续承担键鼠控制和旧协议兼容，但不能直接视为新框架：

| 已有实现 | 与目标的差距 | 源码入口 |
| --- | --- | --- |
| Synergy/Barrier 协议 1.8，默认 TCP 24800 | 新增消息需要明确协商，旧端不保证忽略未知消息。 | [ProtocolTypes.h](../../src/lib/deskflow/ProtocolTypes.h)、[ClientProxyUnknown.cpp](../../src/lib/server/ClientProxyUnknown.cpp) |
| Text、HTML、Bitmap 三种剪贴板格式 | 缺少文件对象、通用格式描述和按需提供接口。 | [IClipboard.h](../../src/lib/deskflow/IClipboard.h) |
| 先 marshall 整体内容，再分块发送 | 不适合大文件范围读取与有界在途数据；当前发送块大小为 512 KiB。 | [ClientProxy1_6.cpp](../../src/lib/server/ClientProxy1_6.cpp)、[StreamChunker.cpp](../../src/lib/deskflow/StreamChunker.cpp) |
| 按活动屏幕安排内容发送与发布 | 需增加独立于当前键鼠焦点的同步服务。 | [Client.cpp](../../src/lib/client/Client.cpp)、[Server.cpp](../../src/lib/server/Server.cpp) |
| DFTR、DDRG 文件消息定义仍保留 | 对应处理为空，不能作为已实现文件传输复用。 | [ClientProxy1_5.cpp](../../src/lib/server/ClientProxy1_5.cpp) |
| Portal 剪贴板支持纯文本与 PNG | 新增 HTML、文件列表及独立后台接入需要单独实现。 | [PortalClipboard.h](../../src/lib/platform/PortalClipboard.h) |

## 模块架构

第一版模块运行在 `deskflow-core` 内，GUI 负责开关、能力状态和传输状态。平台线程、文件工作线程和网络执行器通过异步请求及结果队列协作，键鼠事件路径不等待文件操作。

```text
系统剪贴板
    ↕
ClipboardBackend
    ↕
ClipboardBroker ─── OfferRegistry
    ↕                   ↑
ClipboardTransport      │ 引用与租约
    ↕                   │
模拟对端或真实对端       RemoteFileProvider
                        ↕
                  RemoteReadService
                        ↕
                  文件数据连接
```

| 模块 | 职责 |
| --- | --- |
| ClipboardBackend | 监听本地变化，枚举条目和格式，读取指定格式，向本地发布远端内容，报告实际能力。 |
| ClipboardBroker | 管理当前公告、来源和顺序，路由请求，抑制回环；不依赖当前活动屏幕。 |
| OfferRegistry | 保存格式描述、文件元数据、提供者引用和租约；不建立完整文件缓存库。 |
| RemoteFileProvider | 将远端文件表示为本机可读取的只读文件对象，提供元数据和读取入口。 |
| RemoteReadService | 按文件标识和偏移读取源端数据，管理请求、打开句柄、并发窗口和取消。 |
| ClipboardTransport | 编解码、能力协商、请求响应关联、身份验证和数据连接绑定。 |
| LegacyClipboardAdapter | 将旧 CCLP/DCLP 接入 Broker；避免新旧路径同时写入同一系统剪贴板。 |

纯 C++20 状态与文件模块已放在 `src/lib/clipboard/`，平台探针仍在 [src/apps/clipboard-prototype](../../src/apps/clipboard-prototype/README.md)。原型提供 `deskflow-file-prototype`、`deskflow-clipboard-probe` 与 `deskflow-file-service`；主工程通过默认关闭的 `BUILD_CLIPBOARD_PROTOTYPE` 选择构建，尚未接到核心进程生命周期。

## 数据与生命周期

一次公告包含 `offerId`、来源设备、服务端顺序 `revision`，以及具有独立 `itemId` 的条目。每个条目可以提供多个 MIME 表示。文件条目携带文件标识、名称、相对路径、类型、64 位大小和可验证的版本信息，目录内容允许通过列表请求逐步获取。

`offerId` 由来源设备标识与本地递增序号构成；`requestId` 和文件句柄另行编号。正式多端连接中，服务端按接收顺序分配 `revision`，不比较不同机器的墙上时钟。

生命周期遵循以下规则：

1. 公告到达后即可发布格式或虚拟文件入口，不等待文件内容下载。
2. 打开文件时，句柄绑定特定公告、文件和版本；随后复制新内容不改变旧句柄的指向。
3. 当前本地剪贴板仍引用的虚拟文件保持可访问；读取中的旧公告通过引用和租约保留。
4. 关闭、取消或租约到期后释放不再使用的句柄和虚拟入口；不能只因收到新公告就移除所有旧文件。
5. 源文件被修改或失去访问权限时返回确定错误。保留打开的文件描述符并不等于拥有不可变文件快照。
6. 自身发布产生的剪贴板事件携带来源标记；哈希只辅助去重，不能吞掉用户再次复制相同内容的操作。

目录和多文件复制需要保留整个公告的操作租约，包括尚未打开的子文件。复制 A 期间改为公告 B 后，A 的后续 `lookup`、目录遍历和 `open` 仍需有效，不能只保留已经打开的句柄。具体续租期限和回收触发条件通过原型确定，并用目录复制用例验证。

## Ubuntu 剪贴板后端

剪贴板后端与 libei/Input Capture 键鼠后端分别选择：

- 合成器提供 data-control 时，直接接入 `ext-data-control-v1` 或 `wlr-data-control-unstable-v1`。
- 当前 GNOME 原型优先验证 XWayland 的 X11 剪贴板桥接，显式建立 X11 连接，使用 XFixes 监听并实现 selection 读取、所有权和延迟提供。
- Portal 作为有会话条件的后端，分别报告读取、发布、后台可用性等能力；不能把 Input Capture 的激活权限当作全天候访问权限。

不能假定默认运行在 Wayland 平台的 Qt `QClipboard` 自动具备 X11 后端的后台能力。后端选择依据运行时探测及实测结果，不只根据发行版名称。

原型至少验证 `text/plain`、`text/html`、`image/png`、`text/uri-list`，并按 Files 需要提供 `x-special/gnome-copied-files` 的复制语义。文件 URI 指向本机虚拟文件入口，不能直接转发 Mac 的绝对路径。

CrossPaste 可用于参考平台接入：其 [LinuxPasteboardService](https://github.com/CrossPaste/crosspaste-desktop/blob/5d4cdf0852f98cf7dad1c3eda043b71d405ad8b7/app/src/desktopMain/kotlin/com/crosspaste/paste/LinuxPasteboardService.kt) 在 data-control 不可用时回退到 X11，写入使用 AWT。它的 [WaylandClipboardSnapshot](https://github.com/CrossPaste/crosspaste-desktop/blob/5d4cdf0852f98cf7dad1c3eda043b71d405ad8b7/app/src/desktopMain/kotlin/com/crosspaste/paste/WaylandClipboardSnapshot.kt) 会主动读取内容快照，因此不直接采用其完整采集流程作为本项目的按需文件框架。

## Ubuntu 按需文件提供层

采用只读 FUSE3 原型，将远端元数据投影为本机虚拟目录和文件。挂载点由当前用户的运行时目录分配，示意为 `$XDG_RUNTIME_DIR/deskflow/clipboard/`；每个公告使用独立目录，路径不作为网络文件标识。

```text
对端 OFFER → 建立虚拟目录项 → 发布本机 file:// URI
                                  ↓
                         Files 执行原生复制
                                  ↓
                  getattr / readdir：获取元数据
                  open / read：请求远端文件数据
                                  ↓
                     Files 写入用户选择的目标目录
```

原型接口包括 `stat`、`listDirectory`、`open`、`read(handle, offset, length)`、`close` 和 `cancel`。`getattr` 与 `readdir` 可以请求元数据，但不请求文件主体；`read` 映射为范围读取。`lseek`、EOF 和错误码按文件系统语义处理。

目标是让应用正常读文件，不额外落盘完整中间文件。需要记录 FUSE/内核读取、预读和网络请求之间的关系；网络请求可做有界合并和预读，不要求与每次系统调用严格一一对应。文件复制可能是乱序或并发读取，不能只支持顺序流。

本机原型已验证：即使禁用缩略图，Files 浏览目录时仍可能为文件类型探测读取文件头。禁止元数据函数主动下载正文，不等于禁止桌面应用发起真正的 `read`；预览和类型探测产生的有界读取都属于按需读取。

虚拟目录只暴露当前公告授权的对象。原型默认拒绝写入，不接受远端提供的任意绝对路径和路径越界；符号链接、扩展属性及权限保真不作为首个原型的完成条件，必须显式处理或拒绝。

## 协议与连接设计

接口消息暂按以下语义划分，名称、线格式与版本号尚未冻结：

| 消息组 | 内容 |
| --- | --- |
| 能力与公告 | CAPABILITIES、OFFER、RELEASE，报告格式、文件操作、并发与大小限制。 |
| 格式读取 | FORMAT_REQUEST、FORMAT_DATA，按公告和条目请求一种格式。 |
| 文件元数据 | STAT、LIST_DIRECTORY，按文件标识读取属性或目录分页。 |
| 文件访问 | OPEN、READ_RANGE、CLOSE，范围请求携带句柄、64 位偏移、长度和 requestId。 |
| 控制与失败 | CANCEL、ERROR，明确失败原因、请求归属和终止状态。 |

原型先使用进程内模拟源，再用同机回环连接验证相同接口。正式接入时，小型控制消息放在协商后的 Deskflow 扩展中，文件字节使用独立 TLS 数据连接，并通过主会话下发的短期凭据及对端身份校验绑定到授权会话。

第一版保留服务端中转拓扑。设置每连接的在途字节预算与请求数上限，遵循接收端背压，不一次性把完整文件分块排入键鼠事件队列。断线时唤醒等待的读取并返回错误，不能让文件管理器无限等待。

增加协议扩展时需同时修改客户端和服务端的版本选择与消息解析。当前的 `1.8` 不能被默认为支持新消息，也不把示例版本号 `1.9` 当作已经分配或实现的协议。旧端通过 LegacyClipboardAdapter 使用原有能力。

## Mac 端接口假设

本阶段假设 Mac 端满足上述公告、格式读取、文件元数据、范围读取、取消与生命周期约定，且能将对端文件提供给 Mac 应用。Ubuntu 测试使用模拟端验证这些约定，不开展 Mac 文件系统扩展或 Finder 粘贴实现。

正式跨端验收时仍需要真实 Mac 实例验证。当前假设不得用于标记 Finder、双机互通或 Mac 部署已经完成。

## 阶段完成条件

| 阶段 | 完成条件 |
| --- | --- |
| 文档与基线 | 固定目标、Ubuntu 范围、Mac 假设、现有实现边界及测试记录方式。 |
| 文件提供原型 | 模拟远端元数据即时变成虚拟文件；Files 可粘贴，实际读取才产生范围请求，无完整中间副本。 |
| 剪贴板接入 | 原生 Wayland 应用与选定后端完成文本、HTML、图片和文件列表的读取与发布。 |
| 服务与传输整合 | Broker、引用管理、回环抑制、并发窗口、取消、断线及源文件变化处理通过用例。 |
| Deskflow 集成 | 新协议正确协商，旧端降级，文件传输期间键鼠正常；Ubuntu 发送与接收均通过。 |

具体用例、测量项和当前状态维护在 [Ubuntu 原型进度](clipboard-progress.md)。

## 参考资料

- [Deskflow 构建说明](build.md) 与 [现有协议参考](protocol_reference.md)。
- [MS-RDPECLIP 文件范围请求](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeclip/cbc851d3-4e68-45f4-9292-26872a9209f2)：借鉴文件大小与范围读取语义，不复制整个 RDP 栈。
- [Clipboard Portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Clipboard.html) 与 [Input Capture Portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.InputCapture.html)：会话授权和激活条件。
- [File Transfer Portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.FileTransfer.html)：本机文件访问与导出，不能替代网络文件读取服务。
- [libfuse 读取示例](https://github.com/libfuse/libfuse/blob/master/example/hello.c)：文件系统偏移读取回调。
