# Deskflow 跨端剪贴板接口契约

版本：v1 草案。更新日期：2026-10-15，Asia/Shanghai。本文供 Ubuntu 与 macOS 两端评审、确认接口语义，尚不表示双方已确认或正式线格式已经实现。

契约覆盖按需读取、whole-offer 租约、内容版本与错误、共享样本和日志。Ubuntu M0 至 M2 已验证固定数据的范围读取、短响应聚合和 Files 粘贴；本轮 M3/M4 组件新增显式结果、本地文件的可检测版本、租约及同用户 RPC/FUSE 的断线与取消。正式跨端线格式和 core 运行时尚未实现，组件通过不代表完整契约已经集成，详见 [本轮结果](clipboard-tests/2026-10-15-m3-m4.md)。

## 标识与数值类型

| 字段 | 语义 |
| --- | --- |
| sessionEpoch | 每次经过认证的主会话生成的新标识，重连不得复用。 |
| offerId | 来源设备、来源进程 epoch 和递增序号组合出的不透明标识，剪贴板替换产生新 offer。 |
| offerVersion | 本公告成员及目录结构的固定版本，存续期间不修改。 |
| fileId | 公告内稳定的对象标识，不以绝对路径作为网络权限。 |
| version | 对象内容版本，句柄打开后固定 version 和 size。 |
| leaseId / handleId | 分别绑定认证主体、会话、公告和对象，不能跨会话使用。 |
| requestId | 会话内唯一请求标识，重试使用新标识，响应必须对应原请求。 |
| size / offset | 无符号 64 位整数；JSON 中用十进制字符串，避免浮点转换损失精度。 |
| length / returnedBytes | 无符号 32 位整数；单次 length 为 1 至协商上限，v1 默认上限 262144 字节。 |

二进制线格式和字段编码待冻结。POSIX/FUSE 等有符号 64 位接口不能表示大于 INT64_MAX 的文件时，发布前返回 UNSUPPORTED_SIZE，不允许截断或转为负数。

## 读取结果

```text
OpenFile(sessionEpoch, leaseId, offerId, fileId, expectedVersion, consistencyMode, requestId)
  → status, handleId, version, size

ReadRange(sessionEpoch, leaseId, handleId, requestId, offset, length)
  → ReadResult {
      sessionEpoch, offerId, fileId, version, handleId, requestId,
      offset, length, status, bytes, eof
    }

CloseFile(sessionEpoch, handleId, requestId)
  → status
```

bytes 为原始字节数组，returnedBytes 等于其实际长度。每个 ReadResult 必须是完整响应；不能把网络残帧当成成功结果。

### 成功和 EOF

- status=OK 时 eof 才有意义。正数短响应合法；bytes 长度小于请求 length 不表示 EOF。
- length=0 或超出协商上限返回 INVALID_ARGUMENT。本地零长度读取不产生网络请求。
- offset>=size 返回 OK、空 bytes、eof=true；空文件也遵循此规则。
- offset<size 时，令 remaining=size-offset。返回长度不得超过 min(length, remaining)，eof=true 当且仅当返回字节到达该版本文件末尾。
- offset<size 且 length>0 时，OK、空 bytes、eof=false 属于 INVALID_SOURCE_RESPONSE。eof 与固定 size 不一致同样无效。
- 先比较 offset 与 size，再用减法计算 remaining，不直接计算可能溢出的 offset+length。UINT64_MAX 偏移对较小文件仍按 EOF 处理。

需要填满一次系统读取时，消费端持续请求剩余部分，直到满足长度、收到有效 EOF 或遇到错误。FUSE 路径不能把短响应直接当作 EOF，否则可能补零或截断。

### 错误和响应校验

错误响应统一为非 OK status、空 bytes、eof=false，关联字段回显请求的对象与版本；不能把实际新版本作为成功数据版本返回。FUSE 聚合期间任何子请求失败，该次系统读取返回错误，不把已聚合的部分数据报告为成功。

接收端校验全部关联标识、version、返回长度、范围和 EOF。取消或超时请求的迟到响应直接丢弃，不能交给新请求或新公告。无效响应形成本地 INVALID_SOURCE_RESPONSE，并隔离问题数据连接。

目标目录可能已经写入先前成功读取的数据；后续错误必须让复制失败，不能将残缺目标报告为成功。EOF、CloseFile 或租约释放确认都不是用户粘贴完成确认。

## 公告租约

租约粒度固定为 whole-offer，保留全部对象、目录成员和后代，包括尚未打开的子文件。它保留访问引用，不自动创建内容快照。

```text
AcquireOfferLease(sessionEpoch, offerId, expectedOfferVersion, requestId, requestedTtlMs)
  → status, leaseId, offerVersion, grantedTtlMs, state

RenewOfferLease(sessionEpoch, leaseId, requestId, requestedTtlMs)
  → status, grantedTtlMs, state

ReleaseOfferLease(sessionEpoch, leaseId, requestId)
  → status, state

ListDirectory(sessionEpoch, leaseId, directoryId, expectedVersion, cursor, requestId)
  → status, stableEntries, nextCursor
```

v1 建议默认 TTL=30000 ms，接收端不迟于每 10000 ms 请求续租；实际 grantedTtlMs 必须返回。源端以单调时钟判断到期；接收端预留网络往返余量，不依赖两机墙上时钟对齐。

### 取得和替换

1. 当前公告持续可取得租约；被替换后仍保留公告声明的 acquisitionWindowMs，v1 建议至少 30000 ms。Acquire、替换与回收在源端串行判定。
2. 接收端必须先成功取得租约，再发布本地文件 URL 或虚拟入口。取得失败不能公布看似可读取的文件。
3. B 替换 A 不撤销 A 已有的有效租约。在取得窗口内仍可获取 A，回收后返回 OFFER_EXPIRED；不能改为返回 B。
4. A 的取得响应若晚于 B 已成为当前公告，不能再用 A 覆盖 B；应释放未被使用的 A 租约。

### 旧目录和续期

activeHandles==0、单个文件 EOF、剪贴板替换，都不足以证明整个目录复制结束。Files 可能关闭第一个文件后，稍后才打开另一个子项。

原生 FUSE 路径没有可靠的整个粘贴完成回调。第一版保守保留已发布过的文件公告，在会话内继续续租，直到接收端能明确释放、用户显式清理或会话终止；不单凭空闲计时提前清理旧公告。能取得明确复制边界的平台，可由操作对象持有独立租约。

租约数量和元数据预算需要协商。达到上限返回 RESOURCE_LIMIT，不能为接受新公告而悄悄撤销已承诺的旧租约。回收优化必须通过“A 目录复制中换成 B，再打开 A 未访问子项”的用例。

### 释放状态

```text
ACTIVE → DRAINING → CLOSED
   ↘ EXPIRED / REVOKED
```

- ReleaseOfferLease 幂等。释放进入 DRAINING，拒绝新 OpenFile 和目录访问；已打开句柄可在剩余有效期内继续读，关闭最后一个句柄后进入 CLOSED。
- DRAINING 可续租以完成已打开读取，但不能恢复新打开。接收端释放前必须处理完未决 Open，并确认不会再打开子项。
- CloseFile 幂等。与 Read 的先后以源端处理顺序为准，已经受理的 Read 可完成，后续 Read 返回 HANDLE_CLOSED。
- 到期进入 EXPIRED，主会话断开或明确撤销进入 REVOKED。已受理操作持短期执行引用，仍受请求期限约束，不能将结果投递给已取消或失效请求。
- 释放成功只说明资源状态改变，不表示用户复制成功。

## 版本与错误

同一公告不修改目录成员、文件身份、size 或 version。ListDirectory 分页属于同一 offerVersion，不能拼接变更前后的目录。新内容使用新公告；旧版本仍可提供时继续服务旧租约，否则明确失败。

| 情况 | status 或事件 | 接收端处理 |
| --- | --- | --- |
| A 被 B 替换 | 新 OFFER 事件 | A 的有效租约仍读 A，不作为读取错误。 |
| 已回收公告 | OFFER_EXPIRED | 不发布旧入口，不替换为当前公告。 |
| expectedVersion 与固定描述不符 | VERSION_MISMATCH | 拒绝请求，不解释成最新版本。 |
| 源对象修改或被替换，无法提供旧版本 | SOURCE_CHANGED | 当前复制失败；新内容必须使用新公告。 |
| 原对象消失且无法通过保留引用读取 | SOURCE_MISSING | 当前复制失败，不打开同名替代文件。 |
| 本次访问被拒绝 | PERMISSION_DENIED | 保留原因，不改用其他路径。 |
| 租约到期 | LEASE_EXPIRED | 新操作失败，需重新取得租约。 |
| DRAINING/CLOSED 上的新打开 | LEASE_RELEASED | 不重新激活租约。 |
| 明确撤销 | LEASE_REVOKED | 终止相关访问。 |
| 关闭句柄上的读取 | HANDLE_CLOSED | 不复用为另一文件。 |
| 主会话断开 | 本地 DISCONNECTED | 未决操作失败，会话对象撤销；重连创建新 epoch。 |
| 新连接收到旧会话请求 | STALE_SESSION | 拒绝旧关联。 |
| 仅文件数据连接断开 | 本地 DISCONNECTED | 主会话和租约仍有效时，可用新 requestId 重试同版本，不能交付残帧。 |
| 请求期限到达 | TIMEOUT | 结束等待，迟到响应丢弃。 |
| 用户或上层取消 | CANCELLED | 不以空成功响应伪装 EOF。 |
| 无效长度或参数 | INVALID_ARGUMENT | 拒绝请求。 |
| 本地接口不能表示大小 | UNSUPPORTED_SIZE | 发布前拒绝，不截断。 |
| fileId 未列入授权公告 | NOT_FOUND | 不开放任意路径读取。 |
| 响应标识、版本、长度或 EOF 不一致 | INVALID_SOURCE_RESPONSE | 当前读取失败，隔离问题连接。 |
| 对象或请求预算超限 | RESOURCE_LIMIT | 明确拒绝，不无界排队。 |
| 其他底层错误 | IO_ERROR | 当前读取失败。 |

断线由本地传输层返回错误，不要求已断开的对端发送响应。控制和文件连接绑定同一认证会话，禁止重连时把旧句柄悄悄绑定到新对象。

Ubuntu 建议将版本变化与失效租约映射为 ESTALE，DISCONNECTED/TIMEOUT/CANCELLED 分别映射为 ENOTCONN/ETIMEDOUT/EINTR，SOURCE_MISSING 为 ENOENT，PERMISSION_DENIED 为 EACCES，HANDLE_CLOSED 为 EBADF，无效响应与 IO_ERROR 为 EIO。macOS 保留相同业务 status 后映射本端错误。

### 强一致性边界

打开文件描述符和读取前后比较 size/mtime，并不能严格证明内容不可变。要求任意并发写入下也不混用新旧字节时，源端必须提供不可变版本、文件系统快照或等价保证，并声明 immutableVersion 能力。

consistencyMode 默认为 immutable；不能满足这一保证时，Open 返回 SOURCE_NOT_STABLE。调用方若允许可检测变更模式，必须明确选择 detectable；检测到变化立即 SOURCE_CHANGED，并失败整个复制，不能静默降级。当前固定生成源天然不可变；真实文件的一致性保证仍需两端实现和验证。

## 共享数据与故障

公用样本为 [fixtures-v1.json](../../src/apps/clipboard-prototype/fixtures-v1.json)，包含固定文件、十个范围向量、预期字节与 SHA-256、六组故障计划。两端读取同一份样本，不分别维护魔数。

mod251-v1 的规则为：第 offset+i 字节等于 `((offset+i) % 251 + seed) % 251`，seed 为 0 至 250。计算时可先对 offset 取模，避免内部加法溢出；只生成经 size 截断的有效范围。JSON 的 size 和 offset 使用十进制字符串。

大文件默认 size=4294967419、seed=97，无需完整落盘。另有 size=268435579 的复制向量，SHA-256 为 `66c250955c4a76ed9d1f05ec6dcee7a978d07de01528e94c32632e75c4df7692`。范围向量覆盖空文件、跨块、EOF 前后与 UINT64_MAX 偏移。

故障按 fileId、offset、phase 精确匹配，不依赖全局请求计数。每个用例使用独立 sessionEpoch，清单中 occurrences=1 表示该计划在此测试会话内最多触发一次，重试不重新触发；phase 为 before_read、before_response 或 during_response。short-positive 只限制匹配 offset=0 的首次响应，后续范围恢复正常；现有原型的 `--source-chunk-size` 则是所有响应的持续上限，两种测试方式不混用。

| 计划 | 预期 |
| --- | --- |
| 正数短响应上限 4093 字节 | OK；未到文件末尾时 eof=false，消费端继续请求。 |
| 文件末尾前返回零字节 | INVALID_SOURCE_RESPONSE。 |
| 延迟 100 ms，期限 20 ms | TIMEOUT，迟到响应不能影响后续请求。 |
| 响应前断线 | DISCONNECTED，无成功字节。 |
| 收到 17 字节残帧后断线 | DISCONNECTED，残帧不交付。 |
| fixture-v1 撤销并替换为 seed=42 的 fixture-v2 | 旧句柄 SOURCE_CHANGED，不返回 v2 字节。 |

两端还需执行旧目录租约、释放、到期、并发和迟到响应用例，不仅校验最终文件哈希，也校验关联标识与状态。

Ubuntu 自动测试继续执行共享数据与范围内容向量。新增 LocalFileSource 测试覆盖租约、版本变化和业务 status；Unix socket 实验通道覆盖正数短响应、超时、断线、残帧、零响应、错误版本和请求关联。新测试用真实临时文件与代理注入相应条件，尚未实现直接由这六组 `faultPlans` 驱动的统一 runner；不能将语义相近的本机测试计作 Mac 共用计划已全部通过。

## 统一日志

使用 JSONL 与 camelCase 字段。请求和响应至少记录 schemaVersion、side、operation、sessionEpoch、offerId、fileId、version、leaseId、handleId、requestId、offset、length、returnedBytes、eof、status、durationUs。不适用的标识用 null，不填其他对象的值。

- size、offset 为十进制字符串；其余长度、时长与预算字段为有界整数。
- elapsedUs/durationUs 使用单调时钟。可附 unixMs 便于人工关联，但不靠跨机时间计算耗时或判断到期。
- 记录 activeRequests、inFlightBytes 和租约状态，区分开始、成功、失败与迟到响应丢弃。
- 默认不记录正文与完整用户路径，使用文件标识和范围关联。

现有 M0 至 M2 日志已有请求标识、范围、返回长度、时长和预算，但没有 sessionEpoch、leaseId、version 和显式 eof，offset 仍为 JSON 数字。它是原型格式，接入正式传输时需要迁移，不能宣称已经符合 v1。

新增源服务的 `readRange` 日志使用显式 epoch/version/lease/eof/status，offset 为十进制字符串；旧生成源与 FUSE 观察日志保留各自格式。当前已回收句柄不保存无界的对象墓碑，因此其错误结果可能缺少 fileId/version；在冻结正式线格式前，需要确定有界关联保留或明确未知关联字段的表示。所有通道日志尚未统一，不宣称完全符合本节。

## 两端确认事项

1. 确认 OK、显式 EOF、短响应、零响应和错误语义。
2. 确认先取得 whole-offer 租约再发布入口，旧目录子项在替换后仍可打开，释放不是复制完成确认。
3. 确认错误、重连和版本边界，各自说明真实文件是否可提供 immutableVersion。
4. 运行同一 fixtures-v1，用统一字段报告结果；故障与租约用例另记实际通过状态。
