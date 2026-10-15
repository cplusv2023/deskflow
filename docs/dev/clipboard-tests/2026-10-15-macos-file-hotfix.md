# macOS 首次双机试用故障修复

基线：`515afa447`。本轮是用户已发生故障的诊断与代码修复，未运行自动化或跨端验收测试。
只读查看正在运行的进程、GUI 日志和用户保留的剪贴板元数据，没有读取用户文件正文。

## 已确认原因

1. Ubuntu→Mac：用户日志显示 `Remote clipboard available for paste`，Finder 没有粘贴选项。
   文件投影已挂载，发布助手仍在运行；现场 `inspect` 显示仅有
   `org.deskflow.clipboard.prototype.origin`，没有 `public.file-url`。
   `writeObjects:NSURL[]` 之后调用旧式 board-level `addTypes/setString`，在本机系统上丢失文件 URL。
2. Mac→Ubuntu：用户复制静态普通文件，元数据读取直接返回 `file:///.file/id=…`。
   将这一文件引用 URL 交给 Qt 转为普通 POSIX 路径，触发源端无符号链接打开保护并报 `SOURCE_CHANGED`。
   这次错误不是用户文件正文发生变更。
3. 连续剪贴板变化时，取消旧 reader 调用 `kill()` 后立即 `deleteLater()`；子进程仍在退出途中，
   因而触发 `QProcess: Destroyed while process ... is still running`。

## 修复

- 为每个文件构造完整 NSPasteboardItem，包含 `public.file-url` 与来源标记，一次 `writeObjects` 发布。
  返回“可粘贴”之前检查实际 item 数、文件类型及来源标记，避免只剩标记仍报成功。
- 在 AppKit 层使用 `filePathURL` 将文件引用 URL 转成实际路径 URL，再交给现有源端身份和版本检查。
  没有关闭 `O_NOFOLLOW`，没有放宽文件变更校验。对无法解析的引用明确返回错误。
- 自身投影路径过滤同时识别 `/var` 与 canonical `/private/var`，防止已解析引用绕过自身来源过滤。
- 取消旧 reader 后保留 QProcess，等 `finished` 回调回收；也处理 Starting 期间取消及 FailedToStart。
  旧 token 继续拒绝迟到结果。

线协议仍是 1.9/profile 1，不需要更新 Ubuntu 才能接收这次 Mac 修正后的文件公告。
Ubuntu 若同步公共 runtime，可一并获得 reader 回收修复。

## 交付

Release 构建、App 依赖和签名检查；不运行 CTest、Finder 粘贴或自动双机回归。
产物为 `dist/macos-clipboard-v1-hotfix/Deskflow.app` 和
`dist/Deskflow-clipboard-v1-hotfix-macos27-arm64.zip`，对应源码包和 SHA256SUMS 一并生成。
运行环境仍为 Apple Silicon / macOS 27+、macFUSE 5.4.0 FSKit；ad-hoc 签名、未公证。
替换时退出旧 App，打开热修复版后重连并重新复制；旧会话中的文件 URL 不可继续使用。
