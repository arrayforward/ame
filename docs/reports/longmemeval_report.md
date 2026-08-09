# LongMemEval-S 基准评测报告

> 数据：LongMemEval-S（500 题，每题独立 haystack ~115k tokens）。
> 题型为官方五类 + 弃答专项；recall@k 按 answer_session_ids 命中计。
> 逐题产物（本地，不入库）：`bench/runs/longmemeval_100.ndjson`、
> `bench/runs/longmemeval_full.ndjson`、`bench/runs/lme_pref30_fixed2.ndjson`、
> `bench/runs/lme_ms30_digest{,2}.ndjson`、`bench/runs/lme_ms12_ircot.ndjson`、
> `bench/runs/lme_agg10_{two_stage,batch_pass1}.ndjson`、`bench/runs/lme_type_sample{,2..6}.ndjson`。

## 一、前 100 题 vs 全量 500 题

| 口径 | n | judge | recall@10 |
|---|---|---|---|
| 前 100（multi 30 + single-user 70） | 100 | **0.590** | **0.820** |
| 全量 500（修复前管线） | 500 | 0.398 | 0.672 |

前 100 题只含两个题型且 single-session-user 占七成（该题型是强项），不能代表全量
难度分布；全量 500 题型分布拉开后 overall 落到 0.398，**分题型看才有诊断意义**。

## 二、全量 500 分题型指标（修复前口径）

| 题型 | n | judge | F1 | recall@10 | 备注 |
|---|---|---|---|---|---|
| knowledge-update | 78 | **0.615** | 0.324 | 0.821 | 最高——SUPERSEDES 双时态机制验证有效 |
| single-session-user | 70 | 0.557 | 0.384 | 0.457 | 强项 |
| single-session-assistant | 56 | 0.482 | 0.334 | 0.571 | 中 |
| temporal-reasoning | 133 | 0.391 | 0.193 | 0.759 | 中 |
| multi-session | 133 | 0.248 | 0.082 | 0.752 | 弱——聚合瓶颈，见第五节 |
| single-session-preference | 30 | **0.000** | 0.012 | 0.233 | 全灭——失败分析，见第四节 |

knowledge-update 第一（0.615）印证了 SUPERSEDES 边 + valid_to 失效 + "新旧都在选新"
过滤的设计预期；preference 全灭与 multi-session 偏弱是两个明确失败点。

## 三、三类题型抽样迭代（早期冒烟，lme_type_sample 1~6）

全量跑之前的 6 轮小样本冒烟（每轮 6 题：knowledge-update×2 / multi-session×1 /
single-session-user×1 / temporal×2），用于管道改动的快速验证。数字供存档：

| 轮次 | knowledge-update | multi-session | single-session-user | temporal |
|---|---|---|---|---|
| 1 | 0.500 | 1.000 | 1.000 | 0.500 |
| 2 | 0.500 | 1.000 | 1.000 | 0.500 |
| 3 | 0.000 | 1.000 | 1.000 | 0.000 |
| 4 | 0.500 | 1.000 | 1.000 | 0.500 |
| 5 | 1.000 | 1.000 | 1.000 | 0.500 |
| 6 | 1.000 | 1.000 | 1.000 | 0.500 |

结论：样本量过小（每题型 1~2 题）只配作冒烟信号——knowledge-update 从 0.5 爬到
1.0 与词典/归一改动同步，与全量结果方向一致；multi-session 在 n=1 上的 1.000 毫无
预测力（全量仅 0.248）。**一切结论以全量与小样本 A/B 为准，冒烟轮次不外推。**

## 四、preference 全灭（0/30）失败分析与修复

### 逐题分类（30 题全量）

23/30 误弃答 + 7/30 答非所问；30/30 金标会话不在 top-10。
反直觉点：金标轮与问题**有词面重叠**（如 "video editing" 在全 haystack 仅出现于
金标 2 轮），并非"偏好陈述与偏好问题零重叠"的直觉假设。

### 根因链（图快照离线定位）

实体档案通道的第一人称映射按 INVOLVES 边数盲选主说话人——实测 assistant（258）
高于 user（250），错选助手实体 → 候选池圈定错位（金标 INVOLVES user，不在
assistant 池）→ 通道返回弱证据并弃答，**遮蔽了标准管道的正确召回**。
消融实证：同一题图快照重放，实体通道关闭时标准管道 recall=1.0；角色感知修复后
通道开启 recall 同为 1.0 且金标 top-1。

### 修复与 A/B（30 题全量，`bench/runs/lme_pref30_fixed2.ndjson`）

修复：角色感知第一人称（优先 user 角色名、排除 assistant 角色名）+ 通道弃权回退
（弱通道结果回退标准管道）+ 弃答门控对内容词精确命中豁免。

| 口径 | judge | recall@10 | 误弃答 |
|---|---|---|---|
| 修复前 | 0/30 | 0.233 | 23/30 |
| 修复后 | **8/30（0.267）** | **0.633** | 14/30 |

遗留：残余失败以"召回但个性化不充分"为主（judge 判回答未体现用户偏好），
写入侧偏好槽位直查为候选方向。

## 五、multi-session 聚合瓶颈：三轮假设存档（含已否证）

multi-session（含聚合类计数/枚举题）judge 0.248 低于平均线。围绕"瓶颈在检索侧
还是答题侧"做过三轮假设验证：

| 假设 | 产物 | 结果 | 结论 |
|---|---|---|---|
| ① 证据摘要整合（digest，三版：纯摘要/双通道/仅供定位） | `lme_ms30_digest{,2}` | judge 0.100 / 0.167 vs 基线 0.200 | **否证**：摘要改写丢失精确细节，诱发"精确但错误"答案；功能已移除 |
| ② IRCoT 迭代检索补证据 | `lme_ms12_ircot` | judge 4/19 = 4/19 零翻转（证据 16/19 有变化） | **否证**：子查询确能补证据，但答案完整性无改善——瓶颈在答题侧聚合，不在检索侧；IRCoT 默认关 |
| ③ 两阶段聚合作答（逐项提取→聚合作答） | `lme_agg10_two_stage` / `lme_agg10_batch_pass1` | 逐条基线 0/14；预过滤+批量组合 3/14（3 题 0→1 零退化） | 部分有效但未达转正线：answer 侧调用 4.8 次/题 vs 逐条 16.2 次/题（约 1/3.4），缓存重放第二遍 0 新调用；瓶颈在阶段一提取召回（平均 3.2 条事实/题），`--answer-mode aggregate` 默认关 |

第三轮是首个正方向：judge 有真实翻转且调用量降到 ~1/3.4，但绝对值（3/14）仍低。
下一步候选：阶段一提取召回增强（预过滤 N 上调 / 提取 prompt 调优）。

## 六、当前遗留

- 全量 500 为修复前口径（偏好题修复未含），待重跑刷新。
- multi-session 0.248 仍是最大短板，聚合答题链（假设③）值得继续投入。
- 前 100 口径（0.590/0.820）与全量口径差异大，引用数字时必须注明样本范围。
