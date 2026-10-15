# Deskflow Ubuntu 按需剪贴板原型进度

更新日期：2026-10-15，Asia/Shanghai。

Ubuntu 已快进合并 `macos` 的 `0736934e6`，继续修复 Linux 构建、GNOME Wayland 接入、
原生采集和 helper 生命周期。当前验证见 [Ubuntu core v1 落地记录](#ubuntu-core-v1-落地记录)，
历史组件表不能代替本轮集成结果。真实 Mac↔Ubuntu 联调仍未完成。

M0 至 M2 已完成同机原型验证：独立 C++20 工具可挂载只读 FUSE 文件、发布 X11 文件剪贴板，并由原生 Wayland Files 正常粘贴。单文件、空文件、目录、Unicode 文件名及 256 MiB + 123 字节文件已校验通过。模拟源按偏移生成内容，没有完整源文件或中间文件缓存。

构建目标和模块约定见 [按需剪贴板构建目标与架构](clipboard-design.md)，操作说明见 [原型 README](../../src/apps/clipboard-prototype/README.md)。M0 至 M2 结果见 [首轮验证记录](clipboard-tests/2026-10-15-m0-m2.md)。M3/M4 的历史组件结果见 [验证记录](clipboard-tests/2026-10-15-m3-m4.md)。本轮原生 GNOME 四格式双向、本地 TLS/runtime 与 Files 粘贴已有实际结果；Mac 实机和真实 Portal 切屏回归仍待完成。

与 macOS 组对齐的读取结果、whole-offer 租约、版本与错误、共用数据和日志见 [跨端接口契约 v1 草案](clipboard-contract.md)。本轮组件已实现显式读取结果、租约和部分故障语义；当前 fork 的线格式已在首版指南冻结；此前同机实验通道不能单独当作已验收的跨机链路。M5 分步入口和验收门槛见 [core 接入计划](clipboard-m5-integration.md)。

## 本阶段职责

- 在这台 Ubuntu GNOME Wayland 机器上验证剪贴板接入、Files 原生粘贴和远端文件按需读取。
- 假定 Mac 端按约定接口可用，先使用同机模拟端或两个本地进程驱动 Ubuntu 测试。
- Mac 开发和 Finder 验证不作为当前阶段前置任务；同机模拟通过不能计作 Mac 与 Ubuntu 联调通过。
- 不采用先完整下载到缓存、完成后才发布文件剪贴板的方案。

## 已完成与待完成

| 工作项 | 状态 | 依据或下一步 |
| --- | --- | --- |
| 用户体验与范围 | 已确定 | 双向共享、消费者读取时传输；Ubuntu 为当前测试端，Mac 为接口假设。 |
| Deskflow 现有实现调研 | 已完成 | 协议 1.8、三种格式、活动屏幕路由、整体序列化和文件传输空处理函数已核对。 |
| CrossPaste 接入方式调研 | 已完成 | 已读取本地源码，确认 data-control 与 X11 回退，以及主动快照读取的区别。 |
| 本机运行环境清点 | 已完成 | 见下表；运行时依赖存在不代表原型可运行。 |
| 原有 Deskflow 构建 | 历史产物已清理 | 原 `build/bin/deskflow` 与 `build/bin/deskflow-core` 的修改时间为 2026-10-15 19:11；提交前已清理构建目录。 |
| 原有单元测试 | 已记录历史结果 | 2026-10-15 19:11 的 CTest 日志中 31 个测试目标通过、0 个失败；原始日志随构建目录清理。 |
| FUSE 开发环境 | 已就绪 | 执行阶段已存在 `libfuse3-dev` 3.18.2，pkg-config 可解析；本轮未安装软件。 |
| FUSE 挂载和虚拟文件读取 | 原型通过 | 普通用户挂载、随机/并发/短响应/EOF 读取、4 GiB 以上偏移及正常退出卸载通过。 |
| GNOME XWayland 桥接 | 四格式双向通过 | GTK4 原生 Wayland 与 XWayland 文本、HTML、PNG、文件列表双向；新 runtime 的 Files 粘贴通过。 |
| 独立原型构建与测试 | 已通过 | 独立和主工程可选目标构建通过；新增 3 个 CTest 测试目标通过。 |
| Ubuntu 分支提交前回归 | 已通过 | 仓库外干净构建主程序与两个原型工具，31 个原有单元测试及 1 个原型数据源测试通过。 |
| Ubuntu 四格式后端 | 隔离与原生双向通过 | 文本、HTML、PNG、文件列表；覆盖 INCR、XFixes、GNOME 无 TIMESTAMP 代理和回环，原生证据见本轮记录。 |
| 本地文件服务 | 组件与独立进程通过 | LocalFileSource、显式 ReadResult、whole-offer 租约、版本检查及有界范围读取；真实文件仅 detectable 模式。 |
| Broker | 文件公告状态组件通过 | ClipboardBroker 的异步动作、先租约后发布、迟到响应、旧公告保留和预算；非文件格式及平台／传输尚未统一接入。 |
| 独立进程文件入口 | Unix socket → FUSE 通过 | 同用户会话、范围读取、空闲续租、短响应、错误与退出取消；不是远端 TLS。 |
| 新协议和 core 集成 | Ubuntu 首轮落地，M5 未整体验收 | 已接入 1.9/profile 1、绑定 TLS、GUI 和 FUSE；原生桥接、本地真实 core 进度见本轮记录，跨机与旧端矩阵待测。 |
| Ubuntu 双向原型验收 | 本地首轮集成通过 | 实际 runtime、TLS、真实 core 双进程和原生 GTK4/Files 已测；真实 Portal 切屏和 Mac 双机待测。 |
| Mac 与 Ubuntu 实机联调 | 当前阶段外 | Mac 按假设处理，不标记为已完成。 |

原有单元测试通过只说明日志中的原有测试目标通过，不证明当前桌面的剪贴板桥接、Portal 权限、FUSE 或按需文件粘贴已经可用。

## 源码与构建基线

| 项目 | 检查结果 |
| --- | --- |
| Deskflow 工作目录 | `/home/jwchen/apps/deskflow` |
| Deskflow 提交 | `c46781f19b1f5e39df67266665084de15f91b01c` |
| CrossPaste 参考目录 | `/home/jwchen/apps/crosspaste-desktop` |
| CrossPaste 参考提交 | `5d4cdf0852f98cf7dad1c3eda043b71d405ad8b7` |
| 历史构建目录 | `build/`，提交前已清理 |
| 构建类型与生成器 | Debug、Ninja |
| 构建开关 | `BUILD_TESTS=ON`、`SKIP_BUILD_TESTS=OFF`、`BUILD_INSTALLER=OFF`、`BUILD_X11_SUPPORT=ON` |
| Qt CMake 路径 | `/usr/lib/x86_64-linux-gnu/cmake/Qt6`，使用系统 Qt |
| 历史基线测试日志 | 原 `build/src/unittests/Testing/Temporary/LastTest.log`，已清理 |
| 历史独立原型构建目录 | 原 `build/clipboard-prototype/`，已清理 |
| 历史主工程可选目标验证目录 | 原 `build/clipboard-integrated/`，仅构建两个原型目标，已清理 |
| 原型测试结果记录 | [M0 至 M2 验证记录](clipboard-tests/2026-10-15-m0-m2.md)；原始临时日志已清理 |
| 当前增量测试 | [M3/M4 验证记录](clipboard-tests/2026-10-15-m3-m4.md)；`build/clipboard-prototype/` 重新构建，8/8 测试通过 |

上述基线信息从本机既有产物读取。2026-10-15 按要求清理构建目录、临时测试文件及原始日志，保留文档中的结果摘要、可复现步骤和共享测试样本。构建产物不提交 Git；后续每次测试仍需单独记录结果。

## 当前 Ubuntu 环境

以下为 2026-10-15 的本机快照，FUSE 开发库一项已按实际执行时更新。后续运行测试时需重新记录关键环境。

| 项目 | 当前状态 |
| --- | --- |
| 系统 | Ubuntu 26.04.1 LTS |
| 桌面会话 | `XDG_SESSION_TYPE=wayland`，`XDG_CURRENT_DESKTOP=ubuntu:GNOME` |
| 显示连接 | `WAYLAND_DISPLAY=wayland-0`，`DISPLAY=:0` |
| GNOME Shell | 50.1 |
| Files | Nautilus 50.2.2 |
| XWayland | 24.1.10 |
| xdg-desktop-portal | 1.21.1 |
| xdg-desktop-portal-gnome | 50.0 |
| CMake | 4.2.3 |
| GCC | 15.2.0 |
| Ninja | 1.13.2，已安装 |
| Qt 开发库 | 6.10.2，Qt 工具及翻译工具已安装 |
| OpenSSL 开发库 | 3.5.5 |
| libei 开发库 | 1.5.0 |
| libportal 开发库 | 0.9.1 |
| X11 相关开发库 | libX11、XFixes、Xtst、xkbfile、xkbcommon、xkbregistry 已安装 |
| FUSE 运行时 | fuse3 3.18.2，`fusermount3` 可找到 |
| FUSE 设备 | `/dev/fuse` 存在；UID 1000 普通用户挂载和卸载已验证 |
| FUSE 开发库 | `libfuse3-dev` 3.18.2 已安装，pkg-config 可解析 |
| Wayland 协议探测工具 | 当前 PATH 中未找到 `wayland-info`；合成器是否公开 data-control 待探测 |

`wayland-utils` 未安装，当前 M0 至 M2 使用已验证的 GNOME XWayland 文件发布路径，不依赖该工具。存在 `DISPLAY` 不代表所有格式都已验证；本轮仅确认原生 Files 消费文件列表的路径。

## 实施顺序

文档基线已建立。实现按下面六个里程碑推进，每个里程碑保存独立的测试证据；前两个功能里程碑优先验证文件按需读取和 Files 原生粘贴，不等待完整网络协议或 Mac 实机。

| 里程碑 | 任务 | 输出与通过条件 | 当前状态 |
| --- | --- | --- | --- |
| M0 | 原型开发准备 | FUSE 开发库、独立构建目标、输入数据和证据目录就绪，用户挂载能力通过。 | 已通过 |
| M1 | 模拟源与虚拟文件 | 元数据不读取文件主体，随机范围和 EOF 正确，内存有界。 | 已通过 |
| M2 | Files 接收方向 | 发布虚拟文件入口后可原生粘贴，无完整预下载和中间副本。 | 已通过，使用同机模拟源 |
| M3 | Ubuntu 发送与格式读写 | Ubuntu 原生应用的四类格式可采集和发布，本地文件可供模拟端范围读取。 | 原生 GTK4 四格式双向和真实文件发送通过 |
| M4 | Broker 与独立进程传输 | 生命周期、回环、并发窗口、故障、取消及旧公告租约通过。 | 组件、RPC/FUSE、单 peer runtime 与旧目录保留通过；完整 TLS 故障矩阵待补 |
| M5 | Deskflow 集成与回归 | 正确协商和降级，文件流与键鼠隔离，U01 至 U15 全部完成验收。 | 首版已接入并通过本地真实 core；Portal、旧端、性能和 Mac 实机待测 |

M1 与 M2 构成第一条端到端原型。M3 的剪贴板采集探针可与 M1 独立开发，但正式整合在 M2 验证后进行。当前 GNOME 后端满足本机目标即可，不把其他 Wayland 合成器的 data-control 支持作为本阶段完成条件。

### M0 原型开发准备

当前机器的 FUSE 开发库已就绪。其他 Ubuntu 环境可按需准备以下依赖，其中 `wayland-utils` 为可选的协议探测工具；本机本轮未执行安装：

```bash
sudo apt install libfuse3-dev wayland-utils
pkg-config --modversion fuse3
wayland-info
```

独立 C++20/CMake 原型已建立在 `src/apps/clipboard-prototype/`，可单独构建，无需 Qt；主工程的 `BUILD_CLIPBOARD_PROTOTYPE` 默认关闭。构建、CTest 和原生 Files 操作命令见 [原型 README](../../src/apps/clipboard-prototype/README.md)。

原型至少提供模拟数据源、FUSE 挂载和剪贴板探针三个可分别运行的角色。输入数据与测试证据放在独立工作目录，挂载点位于当前用户的运行时目录，不覆盖现有文件。M0 用最小 FUSE 样例验证普通用户挂载、读取、退出和卸载，不使用 `allow_other` 扩大可见范围；正式虚拟文件提供层在 M1 实现。

完成条件：开发库可解析，原型可编译，U01 通过。保留当前 Deskflow 的构建配置和测试记录作为回归基线。

### M1 模拟源与虚拟文件

先实现 `FakeRemoteSource`，提供元数据、目录列表、打开句柄和 `read(offset, length)`。数据可来自专用测试文件，也可按固定规则生成；生成模式能验证 4 GiB 以上偏移而不要求先生成同样大小的磁盘文件。请求记录必须能证明实际读取位置和长度。

再实现只读 FUSE 投影，先覆盖普通文件和目录所需的 `getattr`、`readdir`、`open`、`read` 与 `release`，并显式拒绝写操作。第一步用命令行校验元数据、跨块读取、随机读取、EOF 和目录内容，暂不依赖剪贴板。

网络或模拟源的短响应不能直接当成文件 EOF。非 EOF、非错误的读取应聚合到所请求的长度，再交给 FUSE；否则普通缓存模式下可能出现补零。实现需遵循 [libfuse 的读取约定](https://libfuse.github.io/doxygen/structfuse__operations.html)。

完成条件：U02、U08 通过，U06 的 64 位范围与缓冲限制通过。仅查询元数据时文件主体请求计数为零；普通读取得到与模拟源一致的字节，不为满足局部请求而先完整预读文件再切片。

### M2 Files 原生接收方向

用最小 X11 selection owner 将 M1 的虚拟文件发布到剪贴板，保持所有权和事件处理存续。提供 `text/uri-list`，并按本机 Files 的要求支持 `x-special/gnome-copied-files`；内容引用本机挂载点中的 `file://` URI。

本机 Nautilus 50.2.2 的 [剪贴板实现](https://github.com/GNOME/nautilus/blob/50.2.2/src/nautilus-clipboard.c) 可作为编码依据：GNOME 文件格式采用 `copy` 加文件 URI 行，不能引入空 URI 行。含空格和 Unicode 的路径需要正确编码。

在原生 Wayland Files 窗口中正常粘贴到目标目录，分别验证单文件、多文件、目录和大文件。关闭预览建立无提前读取的基线，再启用预览记录额外请求。若桥接失败，先定位格式公告、所有权和数据请求，不绕回完整文件预下载。

Files 可能尝试复制优化和元数据操作；未实现的操作应返回明确的回退或不支持结果，不能伪报成功。挂载只读也不应意外让目标文件永远只读，需要检查粘贴结果可正常编辑。

完成条件：U03、U07 及 U12 的接收方向通过；目标内容校验一致，复制开始前不准备完整文件，复制过程中不另写完整中间副本，记录峰值 RSS 和读取请求时间线。

### M3 Ubuntu 发送与格式读写

建立显式 X11 连接与 XFixes 监听，枚举 `TARGETS`，处理所有权变化和 selection 数据传输。评估复用 Deskflow 现有 `XWindowsClipboard` 的转换与 INCR 实现，但使其脱离 `XWindowsComputer` 键鼠后端的选择；运行 Wayland 键鼠后端时也需要独立使用剪贴板后端。

分别验证原生应用复制文本、HTML、PNG 和文件列表的读取，以及模拟对端公告这些格式后的本地发布。提供大尺寸图片时验证 INCR；X11 INCR 分段和远端文件 READ_RANGE 分开处理。

文件发送方向将本地文件列表转换为受公告约束的 fileId，模拟端只能通过这些 fileId 请求属性和内容。使用范围读取提供字节，不在复制时完整读入内存。只测试文件路径字符串可读不能算发送方向通过。

完成条件：U04、U05 与 U14 的后端部分通过；模拟端能读取 Ubuntu 源文件的随机范围并校验，抑制自身写入回环，不依赖鼠标越界或活动屏幕。

### M4 Broker 与独立进程传输

将已验证的后端和 FUSE 入口接到 `ClipboardBroker`、`OfferRegistry` 与 `RemoteReadService`。用同机两个进程通过回环连接完成公告、元数据、范围请求、错误和取消，替换进程内模拟调用；此时不要求接入现有 Deskflow 消息解析器。

设置有界并发和在途字节预算，记录每个请求的生命周期。故障注入覆盖慢响应、短响应、乱序、断线和源文件变化；取消结合超时、关闭和中断检查，不能假设 Files 的取消动作总会立即产生一条对应的网络 CANCEL。

对公告 A 的目录复制在公告 B 到达后继续执行，验证 A 中尚未打开的子文件仍可访问。明确租约续期和清理策略，让当前剪贴板、旧复制任务和等待请求分别持有需要的引用。

完成条件：U09、U10、U11、U13 通过，U06 的独立进程内存与在途限制通过，U14 完成双向回环验收。此阶段结果仍属于 Ubuntu 与模拟端互通。

### M5 Deskflow 集成与回归

将新服务绑定到 `deskflow-core` 生命周期，平台、文件和网络回调通过线程间队列衔接。新增经协商启用的控制消息及独立文件数据连接，落实主会话身份关联、连接关闭和资源回收；不向旧 1.8 对端直接发送新消息。

通过 `LegacyClipboardAdapter` 统一系统剪贴板发布，替换新端之间依赖活动屏幕的发送策略。GUI 先显示后端能力、共享开关和错误状态，不承担文件数据搬运。

完成条件：重新执行原有 Deskflow 测试，完成 U15，并将 U01 至 U14 用集成后的路径回归。记录大文件期间键鼠响应、旧端兼容性和停止后的资源状态。只有 Ubuntu 发送、接收两条链路均完成，才能将 Ubuntu 原型标记为完成；真实 Mac 联调另行记录。

## 下一次开发任务

1. 与 Mac 使用兼容的 1.9/profile 1 构建做实机双向联调，分别交换 server/client 角色；先复用已通过的文件与四格式样本。
2. 在真实 GNOME Portal core 会话中验证切屏、休眠、断线和总共享开关。互斥保护和 helper 生命周期已有代码及隔离测试，但不等于 Portal 交互验收。
3. 完成旧 1.8 握手/剪贴板回归、大文件期间键鼠延迟与内存测量，再评估保留挂载达到 8 个后的安全回收交互。

Unix socket 探针仍仅限同用户本地测试；新 runtime 已使用认证绑定的独立 TLS 通道，多端中转仍不在首版范围。

## Ubuntu 验收矩阵

下表区分已验证路径、部分覆盖和后续阶段。文件内容校验通过后才能把对应复制任务标为成功，模拟结果不能计作实机跨端结果。

| 编号 | 场景 | 通过条件 | 状态 |
| --- | --- | --- | --- |
| U01 | 本用户挂载与卸载 | FUSE 可挂载、读取、正常卸载；退出后无遗留工作进程。 | 已通过 |
| U02 | 元数据与文件入口 | 仅调用 stat/readdir 或发布入口时，不请求任何文件主体字节；Files 自身类型探测的 read 另记。 | 已通过 |
| U03 | Files 单文件粘贴 | 从远端虚拟源原生粘贴，实际读取触发 READ_RANGE，目标内容校验一致。 | 已通过 |
| U04 | 原生 Wayland 剪贴板采集 | 文本、HTML、PNG、文件列表分别被正确捕获，记录使用后端。 | 原生 GdkWaylandDisplay 四格式采集通过；runtime 原生源→TLS 通过 |
| U05 | 发布到原生 Wayland 应用 | 对应格式可粘贴；文件列表使用本机虚拟 URI；文本和文件路径不会混淆。 | GTK4 四格式双向通过；runtime 的 Nautilus 正常粘贴通过 |
| U06 | 大文件与内存预算 | 文件大小和偏移覆盖 4 GiB 以上；在途字节与缓冲受限，不出现完整中间副本。 | 部分通过：4 GiB 以上范围、256 MiB 实际复制；更大完整复制待测 |
| U07 | 多文件和目录 | 多层目录、空文件、空目录、Unicode 与空格名称能正常复制。 | 已通过当前样本 |
| U08 | 随机与并发读取 | 非零偏移、跨块、EOF、乱序及并发请求返回正确字节和长度。 | 已通过模拟源与 FUSE 测试 |
| U09 | 再次复制和回环 | A 传输期间复制 B，旧句柄继续读 A，A 目录中尚未打开的子文件也能继续 lookup/open；新粘贴对应 B，自身发布不反复回传。 | 实际 runtime、真实 core 双进程和原生源模式通过；Mac 实机待测 |
| U10 | 取消与断线 | 用户取消或连接断开后，读取及时结束并返回明确错误，释放句柄与请求。 | RPC/FUSE 超时、断线、进程退出取消通过；Files 用户取消和 core 断线待测 |
| U11 | 源文件变化 | 删除、修改或权限变化有确定错误或已定义版本行为，不静默混合内容。 | detectable 模式源服务与 FUSE 错误通过；无 immutable 快照能力 |
| U12 | 预览与按需语义 | 分别开关预览测试，记录提前读取来源；类型探测与缩略图读取均按真实 read 处理。 | 已通过 never/always 原生 Files 测试 |
| U13 | 当前剪贴板与旧引用回收 | 被当前剪贴板或打开句柄引用的入口保持有效，无引用后才清理。 | 组件、runtime 旧目录/续租/停止清理通过；保留挂载上限与复制完成后的回收交互待完善 |
| U14 | 双向与焦点独立 | Ubuntu 与模拟端双向公告和读取，不依赖 Deskflow 当前活动屏幕。 | runtime/真实 core 在不切屏时双向通过；真实 Portal 切屏回归待补 |
| U15 | Deskflow 回归 | 文件读取期间记录键鼠响应；新旧端协商正确，旧端未收到无法解析的新消息。 | 待执行 |

文件系统原型不预设准确的吞吐或延迟阈值。首次运行记录吞吐、首次读取延迟、峰值 RSS、在途字节和并发请求数，随后制定有依据的预算。任何阶段都不能只因复制最终成功就省略内存与中间落盘检查。

## 测试证据记录方式

每次测试记录日期与时区、代码提交、桌面与后端、用例编号、模拟端模式、输入文件大小及校验值、执行步骤、预期和实际结果、日志位置及遗留问题。

按需文件日志至少关联以下字段：

```text
offerId  fileId  handleId  requestId
operation  offset  length  returnedBytes
startTime  duration  status
inFlightBytes  activeRequests
```

单独记录元数据请求和文件主体字节。对“没有完整预下载”的判断结合请求时间线、进程 RSS、临时目录写入和虚拟文件访问日志；不能只凭一次网络流量截图得出结论。

后续每条进展补充可复现步骤与证据。失败、跳过和未执行分别记录，模拟结果与真实双机结果分别记录。

## 进展记录

### 2026 年 10 月 15 日文档基线

- 确定以 RDP 式按需提供为目标，排除完整文件预下载作为发布前提。
- 明确当前仅负责 Ubuntu 原型，Mac 接口可用作为本阶段假设。
- 完成 Deskflow 与 CrossPaste 相关源码调研，建立模块、生命周期和协议语义设计。
- 确认当前桌面为 GNOME Wayland，XWayland 与 FUSE3 运行时存在，FUSE 开发库及实际挂载能力尚待准备。
- 读取既有构建配置、二进制文件和 19:11 的 CTest 日志；日志内 31 个测试目标通过，0 个失败。本次未重新构建或运行这些测试。
- 建立架构文档、进度文档和 Ubuntu 验收矩阵。新的原型代码、挂载测试、文件粘贴测试和跨端联调均未开始。
- 将实施顺序细化为 M0 至 M5，补充逐阶段产物、验收条件与第一批代码范围；当前仍只更新文档，里程碑未开始执行。

### 2026 年 10 月 15 日 M0 至 M2 验证

- 执行时 FUSE 开发库已安装，完成普通用户最小 FUSE 样例挂载、读取及退出卸载。
- 实现独立模拟源与 FUSE 工具、X11 文件剪贴板探针及原生 Files 粘贴辅助脚本；默认不启用主工程可选原型目标。
- 新增 3 个 CTest 测试目标，覆盖生成数据、短响应聚合、元数据零正文读取、随机和并发读取、EOF、4 GiB 以上偏移及 X11 格式转换。
- 通过 AT-SPI 调用原生 Wayland Nautilus 的 `view.paste`，实际复制单文件及批量文件目录；256 MiB + 123 字节文件和其他样本的 SHA-256 均一致，目标文件权限为 0644。
- 复制 272642295 字节主体期间，模拟文件源进程 VmHWM 为 5112 KiB；本次请求最大 4096 字节。没有完整源文件或应用层中间文件副本。
- 原生预览测试确认：never 模式仍有文件类型探测的头部读取；always 模式额外读取 189 字节 SVG。原设置 local-only 已恢复，三个专用测试窗口已关闭，测试挂载和发布进程已清理。
- M0 至 M2 达到同机原型目标；M3 至 M5 和真实 Mac 联调未开始。详细证据见 [验证记录](clipboard-tests/2026-10-15-m0-m2.md)。
- 根据 macOS 组问题补充接口契约 v1 草案，以及共享 fixtures-v1 数据与十个范围向量；显式 ReadResult、租约、版本变化和断线注入仍属于后续阶段。

### 2026 年 10 月 15 日 Ubuntu 分支整理

- 创建 `ubuntu` 分支，将 `origin` 改为个人 fork `https://github.com/cplusv2023/deskflow.git`。
- 清理旧 `build/`、`tmp/`、最小 FUSE 样例及本轮临时脚本和生成文件；保留源码、共享样本、重跑步骤及文档结果摘要。
- 在仓库外使用系统 Qt 6.10.2 干净构建 Deskflow 和两个原型工具，启用 `BUILD_CLIPBOARD_PROTOTYPE=ON`、`BUILD_TESTS=ON`，关闭安装包生成。
- 本轮重新执行原有单元测试，31 个目标全部通过；独立原型默认数据源测试通过，包含共享样本的十个范围向量。本轮没有再次操作桌面剪贴板或运行原生 Files 粘贴。
- 临时构建及原始日志在验证后清理，提交中仅包含源码、测试样本及文档。

### 2026 年 10 月 15 日向 M5 推进的 M3/M4 增量

- 新增独立 X11 格式提供与读取、双向 INCR、XFixes 监听、owner/timestamp 固定及文件 URI 采集；隔离 Xvfb 测试不接触当前桌面剪贴板。
- 新增 LocalFileSource：固定授权清单、不透明 ID、64 位范围、显式 EOF/status、整份公告租约与源变化错误。普通文件默认拒绝 immutable，只允许显式 detectable。
- 新增同用户 AF_UNIX 服务和接收投影；真实文件列表可采集后按范围提供，FUSE 不生成完整内容缓存；有界短读、连接／请求期限和退出取消已验证。
- 新增 ClipboardBroker 状态组件：无网络／平台调用，动作列表驱动；先取得租约、迟到公告防覆盖、原 owner 回环抑制、旧目录引用保留与预算。
- 将 M5 的实际入口、Portal 冲突、版本选择、旧端边界和 TLS 会话绑定要求列入接入计划。未修改现有协议 minor，未向旧端增加消息，未把组件测试记作 core 集成或 Mac 联调通过。
- 独立构建 8/8 CTest 通过、0 跳过；主工程三个可选目标编译通过。详见本轮验证记录，原生持续同步与 M5 仍按上述门槛推进。

## Ubuntu core v1 落地记录

2026-10-15，基于 `0736934e6`，在本机 Ubuntu 26.04.1 / GNOME 50.1 / Nautilus 50.2.2 实施。
本节是当前状态，上文 M0–M4 历史记录及未更新的验收矩阵不作为本轮集成结论。

### 实现

- 合并 Mac 集成基线，保留统一的 1.9/profile 1、FileTypes、LocalFileSource、LocalFileRpc、TLS 和 FUSE；本轮没有修改网络 schema。
- 修复 `XWindowsClipboard::motifFillCache()` 错用 ICCCM 局部变量导致的 feature-on 编译失败。
- 允许 GNOME Wayland + XWayland；无 DISPLAY 或其他 Wayland 桌面明确报告不可用。GUI tooltip 和 Ready 状态显示实际后端。
- PortalClipboard 的读取、缓存更新、claim 和旧选择传输持有 `LegacyClipboardGuard`，与新 runtime 的启停互斥；切屏入口也跳过旧读取。已经开始的旧操作先结束，再交接，新 helper 清理后才恢复旧路径。
- GNOME 原生代理可能拒绝 TIMESTAMP。新增内部 `watch --capture`，在持续监听 XFixes 的同一连接采集所有表示，拒绝途中 owner/generation 改变；文件列表采集也改为同一连接。小格式总正文 16 MiB，内部 base64 管道缓冲 32 MiB，文件主体不走这个路径。
- 发布用采集时的 X server 时间执行 SetSelectionOwner，避免同一代理窗口发生新复制后被旧结果覆盖；补了无 TIMESTAMP 的过期 claim 回归。
- Ubuntu 客户端初始只监听、记录原 owner，启动时的既有选择由 server 公告；监听完成后才显示 Ready。修复真实 core 重连时旧初始选择覆盖新复制的竞争。Mac 客户端的启动公告仍需实机联调核对。
- core 启动 helper 时重置信号屏蔽集，避免继承 Arch 的 SIGTERM 屏蔽，修复正常停止及父进程死亡后的 FUSE 清理。
- 补齐 read 日志的 epoch、offerId、fileId、version、leaseId、handleId、requestId、offset、length、eof；不记录正文或认证密钥。
- 恢复合并时遗漏的本地 RPC、四格式、RPC→FUSE 测试注册；新增实际 runtime 和真实 core 两种回归入口。

### 构建和试用

本轮使用 GCC 15.2、系统 Qt 6.10.2、OpenSSL 3.5.5、FUSE3 3.18.2。未安装系统软件；临时解包的 Xvfb 与 clang-format 20.1.0 仅用于验证。

```sh
cmake -S . -B build/ubuntu-clipboard-v1 -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/usr \
  -DQt6_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt6 -DOPENSSL_ROOT_DIR=/usr \
  -DBUILD_CLIPBOARD_FILES=ON -DBUILD_INSTALLER=OFF \
  -DBUILD_TESTS=ON -DBUILD_TESTING=ON -DSKIP_BUILD_TESTS=ON \
  -DCLIPBOARD_PROTOTYPE_FUSE_TESTS=ON
cmake --build build/ubuntu-clipboard-v1 --parallel 4
XVFB_BINARY="$PWD/build/ubuntu-test-tools/usr/bin/Xvfb" QT_QPA_PLATFORM=minimal \
  ctest --test-dir build/ubuntu-clipboard-v1 --output-on-failure
```

X11 隔离测试需要 Xvfb；本机已解包至 `build/ubuntu-test-tools/usr/bin/Xvfb`，没有安装到系统。
其他机器可将 `XVFB_BINARY` 指向自己的 Xvfb；安装在 PATH 时可省略该环境变量。
原生测试必须显式运行，不注册到普通 CTest；会替换桌面剪贴板并打开专用测试窗口：

```sh
/usr/bin/python3 src/apps/clipboard-prototype/test_gnome_clipboard.py \
  --probe build/ubuntu-clipboard-v1/bin/deskflow-clipboard-probe
XVFB_BINARY="$PWD/build/ubuntu-test-tools/usr/bin/Xvfb" \
  /usr/bin/python3 src/apps/clipboard-prototype/test_core_runtime.py \
  --harness build/ubuntu-clipboard-v1/bin/clipboard-core-harness \
  --probe build/ubuntu-clipboard-v1/bin/deskflow-clipboard-probe --native-gnome
```

原生测试需要系统 PyGObject、GTK4 与 AT-SPI introspection，第二条另需 Xvfb。
实际使用从 `build/ubuntu-clipboard-v1/bin/deskflow` 启动 GUI，按 [首版指南](clipboard-core-v1.md) 配置双方指纹、TLS 客户端认证和实验开关；helper 由 core 自动启动。

### 已取得的证据与边界

| 验证 | 结果及实际覆盖 |
| --- | --- |
| Feature-on | Ubuntu 完整工程、core、GUI、helper 和测试程序构建通过。 |
| Feature-off | Ubuntu core 和 GUI 构建通过。 |
| 完整 CTest | **43/43 通过，0 失败、0 跳过，13.06 秒**；包含公共库 5 项、原有 31 项及原型/runtime/真实 core 回归。 |
| `clipboard_x11_formats` | 隔离 Xvfb 四格式、INCR、owner/generation、文件采集；新增无 TIMESTAMP snapshot、过期 claim、base64 有界小格式检查。 |
| `clipboard_core_runtime` | 两个私有显示，实际 PeerLink/TLS/FUSE；测试宿主提供合成主会话绑定。双向四格式和文件、无文件预读、A→B 后旧目录、源变化、断线和退出；旧操作互斥交接、错误绑定不消费 listener、合法后续绑定通过。 |
| `clipboard_core_session` | 两个真正的 deskflow-core，私有 X11/配置/证书/信任库/TMPDIR；主连接双向 TLS、1.9/DCAP、导出密钥绑定、双向复制、普通 IPC stop、重连和 SIGKILL 父进程后的 helper 退出/FUSE 卸载。没有合成主连接绑定。 |
| 原生四格式 | GdkWaylandDisplay 的 GTK4 测试窗口与 XWayland 文本、HTML、PNG、文件列表双向字节一致；PNG 经 INCR。不是只在 Xvfb 改环境变量。 |
| 原生 runtime | 一端本机 GNOME、另一端私有 Xvfb，GTK4 原生源→实际 runtime/TLS/FUSE；反向由 Nautilus `view.paste` 正常粘贴且内容相同。宿主仍提供合成主会话绑定，不计作 Portal core 或 Mac 实机通过。 |

实际 core 文件样本包含 4 MiB + 10 字节文件、空文件、空目录和 Unicode 子项。A 的打开句柄关闭后、B 已发布时，仍能打开 A 中未访问的子项并逐文件校验 SHA-256。普通文件变化返回错误，不承诺不可变快照。

本机原始日志位于忽略的 `build/ubuntu-clipboard-{configure,build,tests,session,native,native-runtime}.log`；
CTest 详细日志在 `build/ubuntu-clipboard-v1/Testing/Temporary/LastTest.log`。构建产物与本机证据不提交 Git。

### 仍需完成

1. Mac↔Ubuntu 真机双向，交换 server/client 角色；Mac 最新原生代码的构建与 Finder 行为由 Mac 组验收。
2. 真实 Wayland Portal core 的切屏/所有权、权限、休眠恢复、总开关和连接 reset。互斥逻辑编译/宿主测试通过不能替代真实 Portal 交互。
3. 旧 1.8 双方向握手及剪贴板兼容、跨网防火墙、完整取消/超时矩阵、大文件期间键鼠延迟和峰值内存。
4. 当前每会话最多保留 8 个接收挂载，达到上限需在旧复制结束后重连。尚无可靠的原生“目录复制完成”通知，不按最后一个句柄关闭或短期闲置自动删除旧树；安全回收交互与测试包仍需完善。

本轮属于 Ubuntu 首版落地，不将 M5 标记为全部完成，也不将已有单机结果写成 Mac↔Ubuntu 已可交付。
