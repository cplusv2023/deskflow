# Deskflow macOS 按需剪贴板进度

更新日期：2026-10-15，Asia/Shanghai。

`macos` 分支基于 `ubuntu` 的 `0f08f3111` 建立，本轮已合并最新 `6a657c5e3`。Ubuntu 原有 M0–M2
代码保留在同一原型目录，平台构建分别选择 X11 和 AppKit。本文记录 Mac
侧真实完成范围，不将 Ubuntu 的 Files 结果当作 Finder 验收。

目标见 [架构](clipboard-design.md)，跨端语义见
[契约 v1 草案](clipboard-contract.md)。本分支的新共享服务位于
[`src/lib/clipboard/`](../../src/lib/clipboard/README.md)，两端统一使用 Ubuntu 的 FileTypes/LocalFileSource/LocalFileRpc；旧 Mac 泛型接口隔离在 experimental。
Ubuntu 已合入内嵌协议与 GUI；仍需同步本轮公共 runtime 的本地监听初始化屏障。

**Mac M0–M2 的基础验收现已通过**：macFUSE 5.4.0 的 FSKit 挂载、挂载后范围
测试和 Finder 原生 Cmd+V 均已实测。Finder 复制了多文件、目录、空目录及
256 MiB + 123 B 大文件，哈希一致；正文读取在粘贴后才开始。详细证据见
[Mac M0–M2 验证记录](clipboard-tests/2026-10-15-macos-m0-m2.md)。
重复 Finder 复制时，源进程 RSS 采样最大值为 15536 KiB；采样器设置 50 ms
间隔，这不是系统记录的实际峰值。Finder 取消与预览对照仍待补测。

## 首版路线：先基于 FUSE 交付（用户最新决定）

用户优先要求推出可用、便于试用和定位问题的版本。基于已有 M0–M2 结果，
**首版选择现有 macFUSE/FSKit 文件提供后端，File Provider 后置**。
首批使用者需要安装 macFUSE 并启用 FSKit 模块；先接受这一部署前提，集中
完成实际 Ubuntu↔Mac 文件传输。此前对普通用户安装体验的顾虑保留为后续
改进方向，不再阻塞首版。此次路线调整不新增或重跑测试。

本轮已把真实文件源、独立 TLS、FUSE 投影、AppKit/X11 提供器接入 core/GUI，完成编译。
已补齐客户端旧剪贴板跳过、监听初始化后处理远端内容及 Mac 旧剪贴板原子交接，
生成内含 Qt/OpenSSL 的 Release App 和 ZIP（Apple Silicon / macOS 27+，ad-hoc 签名、未公证）。
构建与分发限制见 [Release 记录](clipboard-tests/2026-10-15-macos-release.md)。
交付范围、启用步骤、固定线格式见 [内嵌首版指南](clipboard-core-v1.md)。
未执行新增测试，尚无真实双机运行结果；M0–M2 历史结论不扩大为新链路 M5 验收通过。

首版以文件复制为主要交付闭环：Ubuntu/Mac 任一端复制真实文件或目录，另一端
通过本地文件入口正常粘贴，读取时才传输；源变更按已确认的 detectable 模式
失败。复用已有文本/HTML/PNG 能力，跨应用格式保真扩展与性能调优后置。
为便于试用和后续优化，集成时提供功能开关、依赖/连接/挂载状态、可定位的
失败原因和带请求标识的日志，并处理退出、断线及重连时的资源清理。

**File Provider 是后续改善部署体验的原生候选**，不依赖 FUSE。采用
`NSFileProviderReplicatedExtension` 可先公告元数据，再由系统按访问请求内容；
Apple 的官方示例也使用本地 HTTP 文件服务器，架构上可以对接 Ubuntu/LAN
文件服务。这需要随 app 打包并签名的 appex、域管理及共享状态，不能仅替换一个
挂载参数。[Apple File Provider 示例](https://developer.apple.com/documentation/fileprovider/synchronizing-files-using-file-provider-extensions)

File Provider 支持 partial range，但 Apple 明确：POSIX 部分读取可触发部分下载，
整文件 clone 或通过 file coordination 读取会请求完整文件。因此，尚不能承诺
Finder Cmd+V 没有整文件中间落盘。后续评估需用最小签名 appex 确认 Finder 实际回调、
系统缓存及峰值磁盘空间、取消、断线与首次部署体验，再决定是否满足产品目标。
[Apple partial-content API](https://developer.apple.com/documentation/fileprovider/nsfileproviderpartialcontentfetching)

**File Promises 保留为补充拖放候选**。本机 macOS 27.0.1 已将纯
`NSFilePromiseProvider` 发布到 general pasteboard，未同时发布 file URL：Finder
普通 Cmd+V 没有生成文件，编辑菜单的“粘贴”禁用，写入回调为零。证据为
`tmp/clipboard-macos/file-promises/finder-result.json`。这个结论只覆盖本机版本和
这次纯 promise 场景，不能扩大成“所有 macOS 版本都不支持”；也不能用自建
receiver 代替 Finder 验收。Apple 的官方示例覆盖拖放路径。
[Apple file-promise 示例](https://developer.apple.com/documentation/appkit/supporting-drag-and-drop-through-file-promises)

## 当前状态

| 阶段 | 已实现和验证 | 尚未完成 |
| --- | --- | --- |
| M0 开发准备 | 基础验收通过；独立 CMake/Ninja 构建、macFUSE3 SDK、本用户 FSKit 挂载及自动测试卸载通过 | 其他 macOS 版本与部署环境未覆盖 |
| M1 文件提供 | 基础验收通过；共享十个向量、短响应聚合、64 位范围及挂载后的随机/并发/EOF/目录读取通过；重复 Finder 复制 RSS 采样最大值 15536 KiB | 系统实际峰值 RSS 与更长时间负载待测；现有结果为定时采样 |
| M2 Finder 接收 | 基础验收通过；原生 Cmd+V 复制多文件、目录、空文件、Unicode 名称和 256 MiB + 123 B 文件，哈希与权限通过；粘贴前正文请求为零 | 取消、预览开关对照及超过 4 GiB 整文件复制待测 |
| M3 Mac 发送与格式 | AppKit 文本、HTML、PNG 延迟提供及读取；本机 TextEdit 纯文本发送、HTML 样式接收和 RTF→HTML 发送通过；Preview PNG 接收/发送逐像素一致；剪贴板文件列表到本地服务和超过 4 GiB 偏移通过 | 多应用、后台权限、跨机格式互通及完整格式边界仍待验证 |
| M4 服务整合 | 保留历史组件验收；本轮收敛到 Ubuntu 公共服务，并接入 AppKit/FUSE、周期续租与独立 TLS，编译通过 | 新组合的实际运行与完整故障矩阵仍待验收 |
| M5 Deskflow 集成 | 首版代码已接入：1.9 协商、主会话绑定 TLS、AppKit/X11 持续监听、FUSE 租约/桥接、GUI 开关/状态、断线清理；Mac Release App/ZIP 已打包并检查依赖和签名 | 按用户要求未运行测试；双机、新旧端兼容和故障矩阵未验收；Developer ID 签名与公证未完成 |

真实文件模式已由用户明确选择：**允许 detectable，检测到修改即失败**。
服务 API 仍默认 immutable，本地文件源不能保证不可变时返回
`SOURCE_NOT_STABLE`；Mac 服务工具要求显式传入 `--consistency detectable`。
文件描述符、size/mtime/ctime 检查不宣称具备快照一致性。

## 构建与自动测试

本次环境：macOS 27.0.1、arm64、Command Line Tools / SDK 27.0、AppleClang 21、
CMake 4.4.4、Ninja 1.13.2。macFUSE 已从 5.1.3 更新到 **5.4.0**，其
`fuse3.pc` 报告 libfuse3 3.18.2，位于 `/usr/local/lib/pkgconfig/`。
主工程使用 Homebrew Qt 6.11.2 和 `/opt/homebrew/opt/openssl@3`；独立原型
不依赖 Qt。文件系统扩展已通过“按类别”界面启用，并以实际挂载确认可用。

```sh
cmake -S src/apps/clipboard-prototype -B build/macos-clipboard -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DPython3_EXECUTABLE=/opt/homebrew/bin/python3 \
  -DCLIPBOARD_PROTOTYPE_DESKTOP_TESTS=OFF
cmake --build build/macos-clipboard --parallel 4
ctest --test-dir build/macos-clipboard --output-on-failure
```

默认不挂载、不修改系统剪贴板。AppKit 测试使用每次独立创建的命名剪贴板，
测试后释放。不要用旧的 `OSXClipboardTests` 替代隔离测试：旧测试会清空
general pasteboard；主工程验证使用 `SKIP_BUILD_TESTS=ON`，再选择测试。

| 测试 | 本机结果 |
| --- | --- |
| clipboard_prototype_source | 通过；共享十个向量、短响应聚合、跨块、EOF、超过 4 GiB 偏移 |
| clipboard_prototype_appkit | 通过；多 URL、多格式、来源标记、延迟读取、源变化拒绝及预算 |
| clipboard_prototype_mac_file_service | 通过；命名剪贴板选文件→独立服务进程→租约/目录/打开/读取/释放；64 位范围与错误 |
| clipboard-service-contract | 通过；包括真实文件变化、旧目录子项、并发预算、租约边界及迟到响应 |
| clipboard-broker-lifecycle | 通过；旧发布结果失效、自身发布识别、相同内容再次复制、会话终止 |
| clipboard_prototype_mount | 通过；独立 FSKit 挂载测试 47 个范围请求，随机/并发/EOF、只读及卸载检查通过；采样 RSS 13440 KiB |

共享服务另以 `-Wall -Wextra -Werror -fsanitize=address,undefined
-fno-omit-frame-pointer` 构建，两个服务/Broker 测试通过。

Mac 编译有三点必须保留：

- `FUSE_DARWIN_ENABLE_EXTENSIONS=0` 选择 libfuse3 可移植回调 API，避免
  `getattr/readdir/statfs/xattr` 的 Darwin 扩展签名差异。
- `NO_SYSTEM_FROM_IMPORTED` 保证 FUSE3 include 路径优先。否则 CMake 的
  `-isystem /usr/local/include/fuse3` 可能让默认路径中的 FUSE2 `fuse.h` 抢先匹配。
- 主工程在根 CMake 启用 `OBJC/OBJCXX`，使已有平台库与可选 AppKit 工具使用
  一致的编译规则；在根目录注册 CTest，避免只发现 `src/unittests` 子树。

主工程构建和已执行的安全回归命令：

```sh
cmake -S . -B build/macos-core -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
  -DPython3_EXECUTABLE=/opt/homebrew/bin/python3 \
  -DBUILD_OSX_BUNDLE=OFF -DBUILD_INSTALLER=OFF -DBUILD_CLIPBOARD_PROTOTYPE=ON \
  -DBUILD_TESTS=ON -DBUILD_TESTING=ON -DSKIP_BUILD_TESTS=ON \
  -DCLIPBOARD_PROTOTYPE_DESKTOP_TESTS=OFF
cmake --build build/macos-core --parallel 4
QT_QPA_PLATFORM=offscreen ctest --test-dir build/macos-core --output-on-failure -j 1 \
  -E '^(OSXClipboardTests|OSXKeyStateTests|clipboard_prototype_mount|clipboard_prototype_x11)$'
```

根目录共发现 35 项测试，本轮执行 33 项全部通过：28 项原有安全测试与五项新增
原型/共享服务测试。排除的两个旧 Mac 测试会分别修改 general pasteboard 和注入
真实按键；挂载另由独立测试执行。串行运行也避免原有 Settings/I18N 测试共用
构建目录中的临时文件。此结果不是新剪贴板功能的 core 集成验收。

## FSKit 挂载与 Finder 验收

Mac 挂载显式使用 `backend=fskit`，不会静默退回内核扩展。
最初使用 5.1.3 的探测未成功，等待 20 秒后仍未挂载；失败诊断保留在
`tmp/clipboard-macos/fsKit-attempt/`。更新 5.4.0 后，在系统设置“通用 → 登录项
与扩展 → 按类别 → 文件系统扩展”启用 macFUSE，随后挂载测试通过。
“按应用”界面的无效开关与维护者记录的系统设置问题一致；采用其建议的
“按类别”入口解决。[macFUSE issue #1194](https://github.com/macfuse/macfuse/issues/1194)

启用挂载测试：

```sh
cmake -S src/apps/clipboard-prototype -B build/macos-clipboard \
  -DCLIPBOARD_PROTOTYPE_DESKTOP_TESTS=ON
ctest --test-dir build/macos-clipboard --output-on-failure -R mount
```

Finder 手工验收使用专用目录；以下发布操作会替换系统剪贴板：

```sh
prototype_run=$(mktemp -d /tmp/deskflow-mac-clipboard.XXXXXX)
mkdir "$prototype_run/mount" "$prototype_run/target"
build/macos-clipboard/deskflow-file-prototype \
  --mount "$prototype_run/mount" --log "$prototype_run/files.jsonl" \
  --large-size 268435579
```

保持挂载进程运行，在另一终端以同一个 `prototype_run` 值发布：

```sh
build/macos-clipboard/deskflow-clipboard-probe publish \
  --log "$prototype_run/clipboard.jsonl" \
  "$prototype_run/mount/hello.txt" "$prototype_run/mount/empty.txt" \
  "$prototype_run/mount/nested folder/" "$prototype_run/mount/large.bin"
```

本轮已在 Finder 专用 `target` 中正常 Cmd+V，并通过文件哈希、目录和目标权限
检查。粘贴前范围请求为零，粘贴后共 1044 次，最大单次 262144 B，最大在途
2097152 B、同时请求八个；目标普通文件均为 0644。首次复制后 RSS 为 12176 KiB；
重复 Finder 复制期间，以 50 ms 为设置间隔的采样最大值为 15536 KiB，记录在
`rss-samples.json`。两者均不能当作系统实际峰值。原型生成源未建立完整磁盘源
文件或完整中间缓存。
证据位于 `tmp/clipboard-macos/finder-20261015/`，具体时序和哈希见
[验证记录](clipboard-tests/2026-10-15-macos-m0-m2.md)。取消、系统实际峰值内存及
预览/文件类型探测对照由后续测试补充。

结束时先关闭专用 Finder 窗口，再停止发布器与挂载进程；如挂载仍存在，
只对本次挂载点执行 `/sbin/umount "$prototype_run/mount"`。确认卸载后再清理目录。

## Mac 真实文件服务工具

`deskflow-mac-file-service` 从文件参数或指定剪贴板捕获文件选择，stdin/stdout
承载有界 JSONL 请求/响应。它是本地测试通道，**不监听网络，不提供身份认证，
不等同于正式 Deskflow 传输**。

```sh
build/macos-clipboard/deskflow-mac-file-service --consistency detectable \
  --path /absolute/path/to/a/test-file
```

显式 `--pasteboard general` 才使用系统剪贴板；测试使用唯一名称。启动输出
`ready`、会话标识、公告和条目元数据；后续请求引用这些不透明 ID。
支持 `acquire/renew/list/open/read/close/release/capture/disconnect`。
64 位 offset/size 编码为十进制字符串；响应字节为 base64；每行上限 1 MiB。
具体请求示例见 `test_mac_file_service.py`。

`capture` 刷新配置的来源。剪贴板 generation 在 URL 提取和文件捕获前后校验，
不把已替换的选择报告为当前公告。旧公告的有效租约继续保留原来的目录成员与文件。
请求串行处理；取消和期限字段用于前置/返回检查，不代表已实现网络请求中途取消。
本地文件源的底层 syscall 仍可能等待设备；销毁共享服务前必须结束所有并发调用。

AppKit 的普通数据读取是同步调用，当前工具不声称可取消其 IPC 等待。
16 MiB 上限限制本工具接受的格式数据，不能限制外部 pasteboard owner 或 AppKit
在返回前的内部内存分配。文件主体不经过这个普通格式数据路径。

本机 general pasteboard 的原生应用补测已完成：TextEdit 复制的纯文本由工具
读出，工具提供的 HTML 在 TextEdit 粘贴后保留样式；TextEdit 复制的 RTF 通过
新增适配转换成 HTML。Preview 从剪贴板接收 PNG，再复制由工具读取，160×96
图像与源图逐像素一致，允许 PNG 容器编码不同。证据与输出位于
`tmp/clipboard-macos/native-formats/`。隔离测试另外覆盖 RTF 的粗体/Unicode、
TIFF→PNG 像素与方向转换，以及拒绝嵌入内容和过大输入。以上属于 M3 的本机
基本路径验证，不代表所有应用、后台权限或 Ubuntu↔Mac 格式互通已通过。

## 历史集成计划与 M5 验收门槛

下面保留此前计划背景；本轮实现状态和试用入口以 [内嵌首版指南](clipboard-core-v1.md) 为准。

1. 以现有 FUSE 后端为首版基础，停止扩展本机可行性实验；File Provider、
   额外格式适配和性能调优后置。本轮仅依照已有结果确定范围，不运行测试。
2. 与 Ubuntu 组对齐共享服务接口和传输格式，统一租约、结果校验及真实文件
   detectable 行为；两端继续使用同一份 `fixtures-v1.json`。
3. 将平台发布、当前公告 Broker、FUSE 文件投影与 whole-offer 周期续租接起来。
   Broker 的 revision 当前只是本地串行顺序；正式多端顺序必须由服务端决定。
4. 共同冻结协议能力门控、线格式和版本分配，再实现认证主会话绑定的独立 TLS
   数据连接。不得向未经协商的 1.8 对端发送新消息。
5. 通过 LegacyClipboardAdapter 统一旧/新剪贴板写入和 core 生命周期，补齐
   功能开关、状态、错误日志及资源清理，形成便于双机试用的首版。
   Ubuntu↔Mac 真机双向、旧端兼容与大文件期间键鼠响应仍是未获得的交付证据，
   不能用本机已有结果替代；本轮不执行这些测试。

在这些步骤完成前，不将独立原型、JSONL 管道、共享库链接或单测通过标记为 M5。
