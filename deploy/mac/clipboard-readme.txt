Deskflow 剪贴板首版试用包

适用：Apple Silicon（M 系列），macOS 27 或更高版本。
已内置 Qt、OpenSSL 和剪贴板助手，无需 Homebrew。

安装和启用：
1. 将 Deskflow.app 拖入“应用程序”目录。
2. 安装 macFUSE 5.4.0 完整版本，并在系统设置启用 macFUSE 的 FSKit 模块（非 local）。
3. 按 Deskflow 提示授予辅助功能、局域网等权限，配置原有键鼠连接。
4. Ubuntu 侧使用本 fork 最新集成代码，采用 X11 或 GNOME Wayland + XWayland。
   两端开启 TLS，服务器验证客户端证书，按 Deskflow 流程信任双方指纹。
5. 两端在设置中勾选“与 macOS / Ubuntu 共享剪贴板（文件、文本和图像）”，重启连接。
   新版启用时，服务器配置中的旧剪贴板开关和大小限制会隐藏。
   使用旧版 Ubuntu GUI 时，仍需保留它的“共享剪贴板”总开关。
6. 状态显示 Clipboard: Ready 后，在一端复制文件，在另一端正常粘贴。
   复制完成前保持连接和源文件不变。源文件变更会使读取失败；断开后需重新复制。

滚轮方向：在“客户端配置 → 鼠标滚轮”中，分别设置水平、垂直的“反转方向”。
保存后连接自动重启，不改变 macOS 系统的自然滚动设置。

原生键位：Super/Windows 键对应 Command，Alt 对应 Option，Ctrl 对应 Control。
Ubuntu 服务器中 Mac 屏幕的修饰键映射应保持同名，避免重复交换 Ctrl/Super。
客户端默认启用“Mac Caps Lock：短按切换输入法，长按开启大写”：
短按切换输入源，长按 0.6 秒开启大写，再按关闭。需在 Mac 启用 ABC 与拼音等输入源。
开启此选项后 Mac 输入法独立于服务器。可在客户端配置中关闭以恢复原有锁定同步。
此行为尚未经过双机验收；若服务器丢失 Caps 的真实按下/松开事件，则无法判断长按。

本版只支持一对一连接，提供文件/目录、文本、HTML、PNG，不支持剪切移动或拖放。
数据连接使用服务器动态 TCP 端口；服务器防火墙需要允许 Deskflow core 的入站连接。

签名：ad-hoc 临时签名，未进行 Developer ID 签名或 Apple 公证。
网络下载后首次打开可能需要在“系统设置 → 隐私与安全性”中单独允许该应用。
无需关闭系统的全局安全检查。

本次完成 Release 编译、依赖打包和签名检查，按要求未运行功能验收测试。
出现问题时请提供两端版本、操作步骤及 GUI 日志中的 clipboard 状态/错误。

源码：https://github.com/cplusv2023/deskflow/tree/macos
详细说明：docs/dev/clipboard-core-v1.md
