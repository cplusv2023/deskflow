# 2026-10-15 macOS 首版 Release 分发记录

按用户要求完成修复和打包，**没有运行验收测试**。未启动 App、core 或 helper，
未执行 CTest、挂载、系统剪贴板操作或跨机联调。

## 修复范围

- `macos` 快进合入 Ubuntu `6a657c5e3`：GNOME Wayland/XWayland、信号屏蔽清理及 Portal 旧剪贴板门控等。
- Mac watcher 增加 `--skip-initial`、初始元数据基线和 `watch_initialized`；客户端不公告连接前的旧内容。
- 公共 runtime 在本地监听初始化后才处理握手后的远端消息、报告 Ready；继续使用有界帧/套接字缓冲。
  初始化超过 15 秒则关闭新通道，清理后恢复旧剪贴板。无协议字段变更，保持 1.9/profile 1。
- Mac 原生旧剪贴板读、写、变化检查与 runtime 激活/退出使用同一互斥门控，避免在途旧写入跨过交接点。
- App 安装部署显式扫描 core 的依赖，补齐 OpenSSL；GUI 显式链接可用的 QtSvg，避免独立安装的 SVG 插件依赖漏打包。
  捕获 macdeployqt 的 `ERROR:`（该工具可能返回成功退出码），依赖部署和签名错误中止打包。
- 两个 clipboard helper 安装并签名；macFUSE 保留为独立安装的系统依赖。
  后续使用 Developer ID 时，只有 FUSE helper 获得加载独立签名 macFUSE 库所需的 library-validation entitlement。

## 构建

本机依赖 OpenSSL 3.6.5 的 Mach-O 最低系统为 macOS 27.0，因此本次明确指定 27.0，
并写入 App 的 `LSMinimumSystemVersion`。该包为 **arm64 / Apple Silicon、macOS 27+**，不是通用架构包。
支持更早系统需要换用相应 deployment target 的依赖重新构建，不能只改 Info.plist。

```sh
cmake -S . -B build/macos-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=27.0 -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
  -DBUILD_OSX_BUNDLE=ON -DBUILD_INSTALLER=ON \
  -DBUILD_CLIPBOARD_FILES=ON -DBUILD_CLIPBOARD_PROTOTYPE=ON \
  -DBUILD_TESTS=OFF -DBUILD_TESTING=OFF
cmake --build build/macos-release --parallel 6
cmake --install build/macos-release --prefix "$PWD/dist/macos-clipboard-v1"
ditto -c -k --sequesterRsrc --keepParent \
  dist/macos-clipboard-v1 dist/Deskflow-clipboard-v1-macos27-arm64.zip
```

完整 Release 构建成功。打包包含 GUI、core、两个 clipboard helper、Qt frameworks/plugins、
core 的 OpenSSL 3 和 Qt TLS 插件依赖的 OpenSSL 4 等库。不要求接收方安装 Homebrew。
检查 49 个 Mach-O 的加载依赖：除系统库和文件 helper 的 `/usr/local/lib/libfuse3.4.dylib`，
全部解析至 App 内。依赖报告随包提供；install ID 不计作加载依赖。
`codesign --verify --deep --strict` 和 Info.plist 格式检查通过。以上只检查构建产物，不代表功能验收。

## 分发

- `dist/macos-clipboard-v1/Deskflow.app`
- `dist/Deskflow-clipboard-v1-macos27-arm64.zip`
- `dist/Deskflow-clipboard-v1-source.tar.gz`（本次提交的源码）
- `dist/SHA256SUMS`

本机没有可用代码签名身份，产物使用 **ad-hoc 签名**，未做 Developer ID 签名/Apple 公证。
这是供首批用户试用的分发包；网络下载后的首次打开可能需要在系统设置中单独允许。
不修改系统全局安全设置。运行仍需正常授予 Deskflow 辅助功能、局域网等权限。
macFUSE 5.4.0 完整安装及其非 local FSKit 模块需预先启用；App 不静默安装/启用系统扩展。

用户启用步骤见 [首版指南](../clipboard-core-v1.md)。Ubuntu 侧应同步本次公共 runtime 修复，
使用同一 fork 的 1.9/profile 1。保持双方 TLS/客户端证书验证，并开启实验性文件共享开关。
Developer ID 公证、旧 macOS 构建和真实双机验收均未包含在本次结果中。
