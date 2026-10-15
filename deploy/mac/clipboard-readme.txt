Deskflow 剪贴板首版试用包

适用：Apple Silicon（M 系列），macOS 27 或更高版本。
已内置 Qt、OpenSSL 和剪贴板助手，无需 Homebrew。

安装和启用：
1. 将 Deskflow.app 拖入“应用程序”目录。
2. 安装 macFUSE 5.4.0 完整版本，并在系统设置启用 macFUSE 的 FSKit 模块（非 local）。
3. 按 Deskflow 提示授予辅助功能、局域网等权限，配置原有键鼠连接。
4. Ubuntu 侧使用本 fork 最新集成代码，采用 X11 或 GNOME Wayland + XWayland。
   两端开启 TLS，服务器验证客户端证书，按 Deskflow 流程信任双方指纹。
5. 两端保留“共享剪贴板”总开关，在设置中勾选
   “Share files with macOS / Ubuntu (experimental)”，重新启动连接。
6. 状态显示 Clipboard: Ready 后，在一端复制文件，在另一端正常粘贴。
   复制完成前保持连接和源文件不变。源文件变更会使读取失败；断开后需重新复制。

本版只支持一对一连接，提供文件/目录、文本、HTML、PNG，不支持剪切移动或拖放。
数据连接使用服务器动态 TCP 端口；服务器防火墙需要允许 Deskflow core 的入站连接。

签名：ad-hoc 临时签名，未进行 Developer ID 签名或 Apple 公证。
网络下载后首次打开可能需要在“系统设置 → 隐私与安全性”中单独允许该应用。
无需关闭系统的全局安全检查。

本次完成 Release 编译、依赖打包和签名检查，按要求未运行功能验收测试。
出现问题时请提供两端版本、操作步骤及 GUI 日志中的 clipboard 状态/错误。

源码：https://github.com/cplusv2023/deskflow/tree/macos
详细说明：docs/dev/clipboard-core-v1.md
