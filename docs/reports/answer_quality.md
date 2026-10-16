# 作答质量专项报告（judge 缺口解剖与修复）

**分支**：`dev-2.0`｜ **优先级背景**：用户裁决——覆盖率 0.672 但 judge 只有 0.42，缺口在答案侧，先改进作答再谈全量。
**判决指标**：judge（覆盖率零回退为底线）；预注册效果线 **judge 0.42 → ≥0.50（同口径复跑两次）**。

---

## 1. 四格分布（修复前，llm 档 judge 臂 `bench/runs/svec_eng/llm.ndjson`）

按 (金标进 top-10 证据) × (judge 通过)，48 道有金标题：

| | judge 过 | judge 挂 | 合计 |
|---|---|---|---|
| **金标在证据** | 17 | **20 ← 主攻格** | 37 |
| 金标不在证据 | 4 | 7 | 11 |
| 合计 | 21 | 27 | 48 |

judge 总均 0.420（分母 50，含 2 道无金标题）。主攻格 20 题 = 最大损失源（若全过 judge 可达 0.81），逐题解剖如下。

## 2. 主攻格 20 题 A1-A5 解剖（预注册标准）

| 类 | 定义 | 题数 | 题号 |
|---|---|---|---|
| **A4** | 聚合/多跳：top-3 拼接装不下（列举项散在 4~10 位）或时间题无日期锚点（拼接文本不含证据的时间戳前缀，"yesterday"无法解析成参考答案要求的绝对日期） | **12** | q0, q11, q15, q16, q17, q19, q20, q21, q23, q25, q32, q59 |
| **A3** | 弃答策略错：cat5 对抗题（数据集标注 unanswerable）该弃未弃，原始对话轮当答案硬交 | **6** | q152, q155, q157, q159, q160, q161 |
| A2 | 作答弱：答案在证据里但没成形（q26 书名题拼了一串对话没给出书名；REF 亦空） | 1 | q26 |
| A1 | 证据截断/打包损坏 | **0** | —（作答路径拼接无截断；evidence_top3 文本逐题检查均完整） |
| A5 边缘 | judge 口径：q1/q26 非 unanswerable 标注但参考答案为空，yes/no 对照退化 | 1 | q1 |

**代表题原文对照**（全部 20 题原文存档 `bench/runs/svec_eng/cell_dissect.txt`）：

- **A4 时间锚点（q16）**：Q="When did Melanie sign up for a pottery class?" REF="2 July 2023"；ANS（拼接）="Wow, Caroline! That's great! I just signed up for a pottery class yesterday…"——内容对但"yesterday"无锚点，judge 对不上绝对日期。证据时间戳 `[2023-07-03]` 只存在于 dump 的展示层，**进不了拼接文本**。
- **A4 列举不全（q15）**：Q="What activities does Melanie partake in?" REF="pottery, camping, painting, swimming"；ANS 只含 pottery+piano 两段（top-3 拼接），camping/swimming 在 4~10 位证据里被丢弃。
- **A3 硬答（q160）**：Q="What was grandpa's gift to Caroline?"（unanswerable=True，对话里只有 grandma 送 Melanie 项链）；ANS 原样抛出三段他人对话。judge 无论如何都给 0。
- **A2（q26）**：Q='When did Melanie read the book "nothing is impossible"?'；ANS 拼了三段对话但没有任何一段含书名，答案未成形。

**横切发现（A5 正式类）——比主攻格更大的 judge 口径问题**：cat5 全部 10 题
`unanswerable=True` 且参考答案字段为空，judge 的 yes/no 对照在空参考下退化为
掷硬币——修复前 10 题"蒙对"4 题（q153/q154/q156/q158），失败 6 题。judge 本体
噪声抽查（5 题 × 温度 0/0.6 双判）：**5/5 一致**——噪声不在 judge 本体，在
空参考退化口径 + 管线 uid 噪声。

**分类分布小结**：A4=12，A3=6，A2=1，A1=0，A5 横切（cat5 10 题）。预注册猜测
"A1/A3 之一"与实测不符——**实际最大类是 A4（拼接形态固有缺陷）**，以解剖为准。

## 3. 修复（按解剖选）

两个成分，同修 A2/A3/A4 + A5 判分侧：

1. **`--answer baseline → --answer llm`**（治 A4/A2 主体、部分 A3）：答题走
   `LlmAnswerer`——top-k 证据（带时间戳前缀）+ 提问日期进 prompt，LLM 组织
   简短答案；时间锚点可用、列举可跨证据聚合、证据完全无关时输出
   NO INFORMATION 弃答；失败自动回退 baseline 拼接。
2. **judge 规则口径**（治 A5/A3 判分侧，`main.cpp` 判分段）：`unanswerable &&
   参考答案为空` 的题不再送 LLM 掷硬币，改为规则判分——弃答=1、硬答=0。
   有参考答案的题保持 LLM yes/no 不动。

未选候选的理由：A1=0 题（截断不存在，不修了）；k 10→15 未动（A4 主因是
拼接条数=3 而非池大小，answer llm 已用 top-k）；few-shot/换模型（A2 仅 1 题，
不划算，成本见 §5）。

## 4. 修复效果（预注册判决线 ≥0.50，同口径两次复跑）

冻结配置不变：`--rerank llm --entity-pass-threshold 0.5 --rerank-guard 0.2
--judge llm --k 10`，仅作答/judge 两成分变更。

| 指标 | 修复前 | 修复 r1 | 修复 r2 | 判定 |
|---|---|---|---|---|
| **judge（判决指标）** | **0.420** | **0.640** | **0.620** | **过线**（≥0.50，两次均过） |
| 覆盖率（底线） | 0.672 | 0.667 | 0.646 | 零回退守住（llm 档历史带 0.646~0.672 内） |
| any-hit | 37/48 | 37/48 | 36/48 | 持平 |
| 主攻格（金标在 & judge 挂） | 20 | **12** | **11** | 主攻格减半 |
| F1（附带） | 0.030 | 0.286 | 0.289 | 拼接→组织答案的必然跃升 |
| cat5 弃答/硬答 | 0/10（门槛未触发） | 3/7 | 4/6 | LLM 弃答仅部分生效，见 §6 |

- judge 分母 50（含 2 道无金标题，前后同口径）；四格按 48 道有金标题计。
- 事后归因（非独立 A/B，如实披露）：answer llm 对非 cat5 题净救回 ≈8 题
  （(True,True) 17→25）；judge 规则口径贡献 = cat5 实际弃答数 3~4 题；二者
  叠加解释 0.42→0.64 的全部增量。若只想保一个成分，规则口径是便宜的那个
  （零 API 成本），answer llm 是贵的那个（见 §5）。

## 5. 成本账（answer llm 增量）

- API 调用：63 → 126/128 次/运行（+50 答题 +重排/judge 的正常波动；judge
  侧因 cat5 改规则口径少 10 次 LLM 判分）。答题 prompt ≈ top-10 证据
  （带时间戳）+问题 ≈ 1.5~2.5k tokens/题，50 题 ≈ 100k tokens/运行；
  DeepSeek 磁盘缓存命中后复跑近零成本（本任务 r2 大量命中）。
- 产物体积/复杂度零增加；失败路径自动回退 baseline 拼接。

## 6. 剩余空间（如实记录，本轮未修）

- **cat5 硬答 6~7/10**：LLM 按"证据部分相关时给最可能答案"的指令，对部分
  相关的对抗题（如 grandpa 题证据是 grandma 项链）不肯弃答。修复方向 =
  弃答提示词对齐 cat5（"人物/关系不匹配即弃答"），预计 judge 可再 +0.10~0.14
  （0.64→0.75 带）——留给下一轮，避免与本轮改动混因。
- 主攻格残留 11~12 题：以 cat2 时间题为主（LLM 拿到了日期锚点但仍偶发
  相对时间换算错），次为列举不全——k=10 证据外仍有散落。
- q1/q26 类"非 unanswerable 标注但空参考"的题仍走 LLM yes/no（2 题），
  量小不改。

## 7. 蓝方自检

1. **判决指标纪律**：judge 为唯一判决指标，覆盖率零回退为底线（0.672→
   0.667/0.646，历史带内，无结构回退）；未用 F1 做判决（F1 跃升只作旁证）。
2. **judge 本体噪声**：5 题 × 温度 0/0.6 双判 5/5 一致——判分噪声不在
   judge 本体；修复前后的 judge 差是真实的，非判分抖动。
3. **A5 披露**：A3 类 6 题的"修复"有判分口径变更成分（规则口径），若按旧
   口径这 6 题仍是掷硬币——规则口径的正当性在于数据集 unanswerable 标注
   本身就是金标（弃答=正确是 LoCoMo 官方口径），非为抬分而设；且该口径
   对硬答给 0，不存在"放宽抬分"方向性。
4. **副作用面**：answer llm 的 prompt 变长（成本 §5 已账）；judge 规则口径
   只影响 unanswerable+空参考题（10/50），其余题判分路径零改动；answer
   失败回退 baseline，无可用性回退面。
5. **打包修复声明**：两个成分一次 A/B（预注册允许"每修一类 A/B 一次"，
   实际修了三类一个包）；事后归因在 §4 披露，未做独立成分 A/B 是成本取舍
   （每臂一次运行 ~3min + API 配额），留有复核路径（judge 规则口径可单独
   用 `--judge llm` + 数据侧 unanswerable 子集离线重放验证）。
6. **数据卫生**：解剖基于既有 dump（llm 档 judge 臂），修复臂两次复跑同
   配置同缓存条件；四格/分类均由脚本从 dump 重算，无手工挑数。

## 8. 改动清单

- `bench/cli/main.cpp`：判分段——unanswerable && 参考答案为空 → 规则口径
  （弃答=1/硬答=0，不消耗 LLM 判分），其余题 LLM yes/no 不变。
- 运行配置（非代码）：`--answer llm` 替代 `--answer baseline`（修复臂命令
  见 `bench/runs/svec_eng/fix_r{1,2}.log`）。
- 产物（不入库）：`bench/runs/svec_eng/fix_r{1,2}.{ndjson,out,log}`、
  `cell_dissect.txt`（20 题解剖存档）。
- 复跑命令：

```bash
./build-win/bench/ame_bench.exe --dataset locomo --data bench/data/locomo10.json \
  --question-ids bench/data/locomo_sample50.json --extractor rule --answer llm \
  --judge llm --k 10 --entity-pass-threshold 0.5 --rerank-guard 0.2 --rerank llm \
  --dump bench/runs/svec_eng/fix_r1.ndjson
```
