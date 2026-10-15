# Ubuntu → macOS 文件 URL 发布修复

基线：`clipboard-v1.0.2`。用户确认 Mac → Ubuntu 可复制，但反向 Finder 没有粘贴选项。
本轮只读取用户保留的剪贴板类型、来源标记、挂载文件元数据以及相关系统日志；未读取文件正文。

## 已确认现场

- 用户运行的是 v1.0.2 App。FSKit 投影文件 `test.txt` 存在，stat 返回普通文件、153 字节。
- 独立 `inspect` 进程看到一个 item，只有 `org.deskflow.clipboard.prototype.origin`，没有 `public.file-url`。
- 00:29:54、00:30:01、00:30:47 等系统日志记录：
  `Sandbox extension data required immediately for flavor public.file-url, but failed to obtain. (-20)`。
  pboard 随后记录 `Entry failed validation`，拒绝该文件 URL。
- 写入进程的本地 pasteboard 检查能看到缓存类型，不能证明 Finder 等其他进程能收到。
  所以此前 `Remote clipboard available for paste` 是假成功；光标可见性警告与此故障没有已证实关联。

## 修改

- 文件发布前解析真实路径（含 `/var` → `/private/var`），检查元数据及可读性。
  FUSE 的 init 回调发送 READY 早于跨进程挂载可见性得到确认；对未就绪元数据进行有界轮询。
  不读取文件正文；检查发生在独立发布助手，不阻塞键鼠事件循环。
- 使用实现 NSPasteboardWriting 的 URL 包装对象，将类型、内容及写入选项委托给 NSURL，
  在同一 item 添加来源标记。不混用发布后的 board-level addTypes/setString。
- core 收到 published 后，以独立助手读取 pboard 实际元数据，核对 changeCount、来源、
  文件数量及每项 public.file-url。只有核对成功才报告可粘贴。
  校验超时、助手启动失败、系统拒绝 URL 或新复制抢占时明确报失败。
- 发布消息按完整 JSONL 解析，避免输出拆包或普通字符串包含 published 造成假成功。
  迟到或已被替换的发布结果不能覆盖当前状态。

系统拒绝 URL 的现象已确认，但尚未通过重新发布定位拒绝中的所有底层条件。
路径规范化、挂载就绪等待及 NSURL 原生发布是本轮修复；不将其描述为已通过现场复制验收。
如果新包仍遇到授权拒绝，应继续依据新日志处理，不应要求用户关闭 SIP 或扩大系统权限。

## 交付与验证

仅 Release 编译、差异检查、依赖和签名检查；遵照用户要求不执行新挂载、剪贴板写入、
Finder 粘贴或双机验收测试。协议保持不变，无需为本轮更新 Ubuntu。
产物：`dist/macos-clipboard-v1.0.3/Deskflow.app` 和
`dist/Deskflow-clipboard-v1.0.3-macos27-arm64.zip`。
仍需 Apple Silicon、macOS 27+、macFUSE FSKit；ad-hoc 签名、未公证。
包含前版原生键位、Caps、滚轮及 Mac → Ubuntu 文件引用 URL 修复。
