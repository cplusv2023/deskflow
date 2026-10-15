# macOS 原生修饰键与远程 Caps Lock

基线：`clipboard-v1.0.1`。目标是 Ubuntu 键盘控制 Mac 时保留 Mac 快捷键及本地输入法，
不将 Ctrl 自动替换为 Command。

## 修饰键

- Super / Windows 键 → Command；Alt → Option；Ctrl → Control。
- 沿用服务器按屏幕配置的修饰键映射。如曾交换 Ctrl/Super，需在 Ubuntu 服务器的
  Mac 屏幕配置中恢复同名映射；客户端不会强行覆盖用户的服务器映射。
- 左右 Shift、Control、Option、Command 分别跟踪，聚合标志取两侧逻辑或。
  同时按住两侧再松开一侧，不再清除仍按住的一侧。大写锁定时同样保留两侧设备标志。
- 若 Ubuntu 桌面先拦截 Super 快捷键，Mac 无法恢复未收到的按键；本次不修改 Ubuntu 桌面快捷键。

## 远程 Caps Lock

客户端新增 `client/macNativeCapsLock`，默认 true，GUI 仅在 Mac 显示，可关闭恢复原有逻辑。
不修改 macOS 系统偏好；使用 TIS 选择输入源、IOHID 读取/设置锁定状态。
需要在 Mac 启用 ASCII 输入源（例如 ABC）和非 ASCII 输入法（例如拼音）。

- 关闭大写时短按 Caps：松开时切换 ASCII / 非 ASCII 输入源。
- 长按 600 ms：通过事件队列单次定时器切到 ASCII 并开启大写锁定，无阻塞 sleep。
- 已开启大写时再次按 Caps：关闭大写，不同时切换输入法。
- 非 ASCII 输入法优先恢复本次会话观察到的上一个；首次尚未记录时使用首个已启用且可选的非 ASCII 输入源。
- 忽略 Caps 自动重复；断线、离开屏幕、销毁时取消定时器及待定短按，不触发输入法切换。
- 按 Mac 当前锁定状态处理后续键，隔离服务器 Caps 掩码，并纠正其对 ASCII 字母大小写的影响。
  不宣称所有非拉丁服务器布局均等价于物理键盘透传。
- 启用时不跟随服务器切换键盘语言，GUI 禁用冲突选项，避免输入法刚切换又被后续按键切回。

Apple 对本地 Caps 短按/长按行为的说明：
https://support.apple.com/en-gb/guide/mac-help/-mchl84525d76/mac
本实现为远程输入提供相同的主要操作习惯，阈值固定为 600 ms，不宣称复现系统全部输入法策略。
协议不变，Ubuntu 必须发送完整 Caps down/up；若服务器配置将 Caps 设为 half-duplex，
或输入端仅提供锁定变化而丢失真实松开时间，则无法判定长按，应取消该服务器配置。

## 验证

遵照用户要求不运行按键注入、输入法切换、双机验收或自动化测试。
执行 Release 编译、代码差异检查、App 动态依赖与签名检查。
App：`dist/macos-clipboard-v1.0.2/Deskflow.app`；压缩包：
`dist/Deskflow-clipboard-v1.0.2-macos27-arm64.zip`。
运行环境仍为 Apple Silicon、macOS 27+、macFUSE FSKit；临时签名、未公证。
实际输入法切换及快捷键兼容性尚未经现场验收。
