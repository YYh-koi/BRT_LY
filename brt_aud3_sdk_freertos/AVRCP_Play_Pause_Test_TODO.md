# AVRCP播放暂停测试待办

## 测试目标

验证收到 AVRCP Paused 后主动发送 AVDTP Suspend，以及无关联迟到 Suspend 的 80 ms 延迟复核和 A2DP Start 恢复机制。

当前测试板不带喇叭，本轮只验收协议、状态机、命令时序和最终状态。PCM 是否有真实音频、卡音、爆音和静音听感留到带音频输出的整机验证。

## 测试准备

- [ ] 确认固件使用本轮生成的 `brlink.a`。
- [ ] 确认 `CONFIG_AVRCP_PAUSED_SEND_AVDTP_SUSPEND` 已启用。
- [ ] 默认关闭 HCI 十六进制详细日志，避免影响临界时序。
- [ ] 保存 ADB 测试脚本输出，记录每次按键的编号和发送时间。
- [ ] 每轮测试开始前保留至少 5 秒日志，结束后保留至少 10 秒日志。
- [ ] 日志中能够看到 `[BT_PLAYTRACE]`、`+PLAYSTAT` 和 A2DP/audio state 回调。
- [ ] 记录手机型号、系统版本、音乐 App、测试间隔和日志文件名。

## P0 必测项目

- [x] **T01 单次正常播放/暂停**

  操作：初始 Paused，点击播放，等待 2 秒，再点击暂停；重复 10 组。

  通过：Playing 时 `active=1`，Paused 时 `active=0`；状态严格交替，没有异常补偿 Start。

- [x] **T02 350 ms 轮次 A**

  操作：先发送 1 次暂停完成初始化，随后执行 15 组“播放 -> 暂停”，共 30 次切换，最终为 Paused。

  通过：统计车机实际收到的状态；最终 `PLAYSTAT=Paused`、`active=0`，保持 10 秒不再出现 Start。

- [x] **T03 350 ms 轮次 B**

  操作：先发送 1 次播放完成初始化，随后执行 15 组“暂停 -> 播放”，共 30 次切换，最终为 Playing。

  通过：统计车机实际收到的状态；最终 `PLAYSTAT=Playing`、`active=1`，保持 10 秒无异常 Suspend。

- [ ] **T04 无关联迟到 Suspend 恢复**

  操作：重复快速播放/暂停，直到日志出现 `event=suspend cmd_seq=0 cmd=0`；至少收集 10 次有效样本。

  通过：每次先出现 `fallback scheduled`；约 80 ms 后复核；仍应播放时最多补发 1 次 Start，不得立即 Start 或循环发送。

- [ ] **T05 最终暂停稳定性**

  操作：快速切换后最后一次固定为暂停，保持 10 秒；重复 30 轮。

  通过：最终 `PLAYSTAT=Paused`、`active=0`；Paused 后不得出现迟发的补偿 Start。

- [ ] **T06 最终播放稳定性**

  操作：快速切换后最后一次固定为播放，保持 10 秒；重复 30 轮。

  通过：最终 `PLAYSTAT=Playing`、`active=1`；Start 不重复，不能持续停留在 `Playing + active=0`。

- [ ] **T07 来电/HFP 保护**

  操作：音乐 Playing 时制造迟到 Suspend，并在 80 ms 复核窗口内触发来电、接听、保持和挂断；重复 20 次。

  通过：HFP 活动期间不得补发 Start；日志应出现 `reason=hfp-active` 或等价的阻止证据；挂断后状态能够通过正常流程恢复。

- [ ] **T08 蓝牙断开/关闭保护**

  操作：产生迟到 Suspend 后立即关闭手机蓝牙、关闭车机蓝牙或断开连接；每种方式至少 10 次。

  通过：Close、Disconnect、disconnecting 或 BT disabling 状态下不得发送 Start，不出现协议错误或状态机循环。

- [ ] **T09 手机主动暂停**

  操作：在手机音乐 App 主动暂停并保持 10 秒；重复 30 次。

  通过：车机最终保持 `Paused + active=0`，不得被 80 ms 复核错误恢复为 Playing。

## P1 边界和恢复测试

- [ ] **T10 80 ms 边界竞争**

  操作：分别使用 60、80、100、120 ms 的暂停保持间隔，每档执行 30 次切换。

  通过：新无关联 Suspend 能取消旧 timer 并重新计满 80 ms；最终状态正确，不出现 Start/Suspend 循环。

- [ ] **T11 多间隔 A/B 对比**

  操作：分别使用 40、100、200、500 ms 执行轮次 A 和轮次 B，每档各 30 次状态切换。

  通过：记录每档车机实际收到的 Paused/Playing 数量；允许手机合并短状态，但每轮最终状态必须正确。

- [ ] **T12 A2DP 断开后重新连接**

  操作：Playing 时断开 A2DP，再重新连接并播放；重复 30 次。

  通过：`disconnecting` 状态能够清除；重连后最终达到 `PLAYSTAT=Playing`、`active=1`。

- [ ] **T13 Start 失败与人工恢复**

  操作：在补偿 Start 前后制造断链或手机拒绝响应；至少收集 10 次 Start 失败样本。

  通过：Start 失败后不得自动循环重发；再次播放或重新连接后能够恢复正常状态。

- [ ] **T14 PLAYSTAT 上报一致性**

  操作：对每轮日志统计 `state`、`report` 和 `+PLAYSTAT` 的数量及状态序列。

  通过：有效状态变化一一对应；不存在连续相同 `+PLAYSTAT`、异常回摆或最终状态漏报。

## P2 兼容性和压力测试

- [ ] **T15 多手机兼容性**

  操作：至少使用 2 款 Android 手机和 1 款 iPhone，并覆盖不同音乐 App；每个组合执行 30 次切换。

  通过：无错误自动恢复、持续 `Playing + active=0`、重复 Start、命令失败或协议异常。

- [ ] **T16 长时间随机压力测试**

  操作：随机执行播放、暂停、来电、HFP、A2DP 断开和蓝牙重连；累计 500 次操作或连续运行 2 小时。

  通过：无 `queue full`、持续 `GetTimePast`、assert、死锁、重复命令循环或长期状态不一致。

## 当前板子最终验收

- [ ] 所有 P0 项目通过。
- [ ] 最终 Paused 时为 `PLAYSTAT=Paused`、`active=0`，尾部 10 秒无 Start。
- [ ] 最终 Playing 时为 `PLAYSTAT=Playing`、`active=1`，尾部 10 秒无异常 Suspend。
- [ ] 一个无关联 Suspend 最多产生一次补偿 Start。
- [ ] 新的无关联 Suspend会重新开始完整的 80 ms 复核窗口。
- [ ] Paused、Close、Disconnect、HFP active 能阻止或取消补偿 Start。
- [ ] `state -> report -> +PLAYSTAT` 状态序列一致。
- [ ] 未出现 `queue full`、持续 `GetTimePast`、assert 或协议错误。

## 后续整机音频验收

- [ ] 验证 Playing 后实际有声音，音量正常。
- [ ] 验证 Paused 后声音立即停止，没有残留音频。
- [ ] 验证迟到 Suspend 恢复过程没有明显卡音或过长静音。
- [ ] 验证快速播放/暂停时没有爆音、杂音或反复断续。
- [ ] 验证来电、挂断和蓝牙重连后的声音恢复正常。

## 结果记录

| 编号 | 状态 | 手机/系统 | 音乐 App | 间隔 | 日志文件 | 结论与异常 |
|---|---|---|---|---:|---|---|
| T01 | 通过（协议/状态机） | 未记录 | 未记录 | 约2.05 s | T01_adb_output.txt；pasted-text.txt | 10组、20次状态严格交替；Suspend/Playing/audio均10/10配对；最终Paused稳定约12.46秒；无fallback、Start或re-suspend；无喇叭，听感未测试 |
| T02 | 通过（协议/状态机） | 未记录 | 未记录 | 配置350 ms，实际0.38～0.43 s | T02_adb_output.txt；pasted-text.txt | 15组30次状态全部传播并严格交替；Suspend/Playing/audio均15/15配对；最终Paused稳定约12.96秒；无无关联Suspend、fallback、Start或re-suspend；无喇叭，听感未测试 |
| T03 | 通过（协议/状态机） | 未记录 | 未记录 | 配置350 ms，实际0.39～0.41 s | T03_adb_output.txt；pasted-text.txt | 15组30次状态全部传播并严格交替；Suspend 15/15、Playing/audio 16/16（含初始化）配对；最终Playing且active=1稳定约13.6秒；无迟到Suspend、fallback、异常Start或循环；80ms正向分支未命中；无喇叭，听感未测试 |
| T04 | 证据不足（正向样本0/10） | 未记录 | 未记录 | 320 ms，1000组 | T04_adb_output.txt；pasted-text.txt | 2001次ADB命令完整；无关联Suspend和fallback均0，未覆盖80ms分支；23次可关联Start及2次re-suspend全部正常，最终Playing且active=1稳定约119.55秒；建议310 ms×1000组复测，仍不足再测315 ms |
| T05 | 待测试 |  |  |  |  |  |
| T06 | 待测试 |  |  |  |  |  |
| T07 | 待测试 |  |  |  |  |  |
| T08 | 待测试 |  |  |  |  |  |
| T09 | 待测试 |  |  |  |  |  |
| T10 | 待测试 |  |  | 60/80/100/120 ms |  |  |
| T11 | 待测试 |  |  | 40/100/200/500 ms |  |  |
| T12 | 待测试 |  |  |  |  |  |
| T13 | 待测试 |  |  |  |  |  |
| T14 | 待测试 |  |  |  |  |  |
| T15 | 待测试 |  |  |  |  |  |
| T16 | 待测试 |  |  |  |  |  |

## HCI 日志启用条件

只有出现以下情况时再打开车机 HCI 日志或抓取手机 btsnoop：

- [ ] 手机界面或 ADB 显示状态已变化，但车机没有对应 AVRCP 状态。
- [ ] Start 返回成功，但没有收到对应的 A2DP Playing。
- [ ] Suspend 返回成功，但 A2DP 长时间仍保持 active。
- [ ] 状态机日志无法解释 Start/Suspend 的重复或顺序异常。

