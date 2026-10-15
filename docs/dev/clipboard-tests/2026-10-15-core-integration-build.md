# 2026-10-15 内嵌首版编译记录

本轮按用户要求不再执行测试。以下仅记录编译检查，**不是双机运行验收**。

- 合并基线：`origin/ubuntu` 的 `ea9190bcd`；收尾再次 fetch 未发现新提交。
- Mac 既有工具链：CMake 4.4.4、Ninja 1.13.2、Qt 6.11.2、OpenSSL 3.6.5、macFUSE 5.4.0。
- `BUILD_CLIPBOARD_FILES=ON`：完整工程构建成功，包含 core、GUI、两个内部 helper、共享库及原有测试可执行文件。
  测试程序只参与编译/链接，未运行。修改后重新编译受影响的 core/GUI/helper 成功。
- X11 helper：使用本机 XQuartz 头文件对 `X11Clipboard.cpp` 与 `clipboard_main.cpp` 做 C++20 语法编译检查，成功。
  这不是 Ubuntu 全工程构建或 X11 运行验证。
- feature-off `CoreClipboard.cpp` stub 语法编译成功；没有重复构建完整 feature-off 工程。
- 源码使用仓库 CI 指定 clang-format 20.1.0；`git diff --check` 无空白错误。

主工程配置：

```sh
cmake -S . -B build/macos-core -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
  -DPython3_EXECUTABLE=/opt/homebrew/bin/python3 \
  -DBUILD_OSX_BUNDLE=OFF -DBUILD_INSTALLER=OFF \
  -DBUILD_CLIPBOARD_FILES=ON -DBUILD_CLIPBOARD_PROTOTYPE=ON \
  -DBUILD_TESTS=ON -DBUILD_TESTING=ON -DSKIP_BUILD_TESTS=ON \
  -DCLIPBOARD_PROTOTYPE_DESKTOP_TESTS=OFF
cmake --build build/macos-core --parallel 4
```

收尾增量构建：`cmake --build build/macos-core --target deskflow-core Deskflow --parallel 4`。
当前产物位于 `build/macos-core/bin/`；二进制和机器本地日志不提交。

未执行：CTest、桌面复制、挂载、TLS 联调、旧端兼容、取消/断线矩阵、性能测量、打包签名。
已有 Finder 和 Ubuntu 组件证据仍分别见历史记录；不能推导本次新链路已运行通过。
现行范围与使用步骤见 [内嵌首版指南](../clipboard-core-v1.md)。
