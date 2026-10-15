# 写时语义增强（write-time enrichment）spike 报告

- 分支：enrich-line（worktree `ame-line-enrich`，基于 dev-2.0 `cf9f726`）；不 push。
- 思路：记忆写入时由 LLM 给每轮事件附 ≤5 个语义标签（同义/改述/上下位），查询侧
  零 LLM——标签以 `[tags: ...]` 挂进事件文本，词法通道与终排 BM25 词面命中即跨改述。
- 时间盒：≤60min（含 API）；产物 `bench/data/locomo10_enriched.json` 与
  `bench/cache_enrich/` 已 gitignored。

## 1. 实现

- `bench/scripts/enrich_corpus.py`（新增）：读 locomo10 全部轮次（实测 **5882 轮**，
  勘察阶段按会话字典长度估的 580 是错的——`conversation` 的键含 speaker/date 元数据），
  每轮一次 LLM 调用（temperature 0、关思考——不关闭时思考烧光 max_tokens 全部变 NONE，
  已修复），逐轮 sha256 磁盘缓存（`bench/cache_enrich/`），故障即停并报告。
  泄漏禁令写进 prompt：不得输出原句已出现的词/短语。
- `bench/src/loader.cpp`（最小改动，引擎核心零改动）：`load_locomo` 读轮内可选
  `tags` 数组，以 `" [tags: a, b, c]"` 附于 text 末尾。
  **挂载方式取舍**：候选有 (a) 拼进事件文本（选用）、(b) 事件 attr/keyword 节点、
  (c) M3 词典边。选 (a)：词法通道 + 终排 BM25 都按事件文本切词，附文尾即天然命中，
  bench 侧一行改动；代价是标签词参与内容分计分（干扰风险见蓝方 §5）。
  (b)/(c) 需动引擎核心，超出 spike 范围——若转正必须迁走（标签不应进内容分，
  应作查询扩展侧的独立通道）。
- `bench/scripts/enrich_ab.py`（新增）：三档×{原始,增强} 预注册对照驱动，
  覆盖率口径与 `bench_compare_sample.py`/`llm_rerank_spike` 一致
  （|gold∩evidence|/|gold| 均值，空金标剔除，n=48；附 any-hit）。

## 2. 对照实验（预注册：bm25/cheap 增强版 Δ>+0.03 且零推理成本 → 成立；≤+0.01 → 不成立）

冻结配置同 spike 前作：`--entity-pass-threshold 0.5 --rerank-guard 0.2`，
sample50，`--extractor rule --answer baseline --judge f1 --k 10`（推理侧仅 llm 档有 LLM，
走磁盘缓存；bm25/cheap 档 api_calls=0 = 零推理成本成立）。

| 档 | 原始 | 增强 | Δ | any-hit(原→增) |
|---|---|---|---|---|
| **bm25** | 0.464 | 0.469 | **+0.005** | 25/48 → 25/48 |
| **cheap** | 0.536 | 0.479 | **-0.057** | 28/48 → 27/48 |
| llm（选跑） | 0.646 | 0.688 | +0.042 | 36/48 → 38/48 |

复跑基线核对：bm25_raw 0.464 = cf9f726 机制1+guard 冻结值（0.443+0.021）；
cheap_raw 0.536 落在 0.54~0.57 带下沿（uid 噪声带内）；llm_raw 0.646 = 前报冻结值。
原始侧可复现，对照有效。

### 判决：不成立（bm25 +0.005 ≤ +0.01；cheap 负增长）

- 改述鸿沟主要不在"写入侧词面"：bm25 档全 50 题仅 2 题移动（q15 -0.25、q27 +0.5，
  净抵消）——标签确实把 q27 金标从 0 拉到 0.5（A 类救回 1 例），但量级 ~1/50，
  远低于 +0.03 线。
- cheap 档 -0.057 是明确的负面信号：特征模型在原始文本分布上训练，标签词拉高
  非金标轮的 bm25/ent_overlap 特征 + 终排内容分被标签词稀释 → q101/q11/q154/q155
  四题回退。挂载方式 (a) 的内容分污染在此直接兑现。
- llm 档 +0.042（0.646→0.688）不构成翻案：预注册判据绑定 bm25/cheap 零推理档；
  且 LLM 重排看见的标签括号文本可能反向辅助语义判断（证据文本含标签=泄漏进
  查询侧通道），该增益不能记在"词面跨改述"账上。
- 按预注册归档：标签挂载进内容通道的写时增强**不成立**，如实归档；产品侧维持
  "默认关 LLM，LLM 离线造数据/监督"现状。

## 3. 写时成本账（一次性，离线）

| 项 | 值 |
|---|---|
| 轮次 | 5882（locomo10 全量） |
| API 调用 | 4066 次实调 + 1816 缓存命中（冒烟与断点续跑），失败 0 |
| prompt tokens | 947,526（~161 tok/轮：模板 ~140 + 轮次文本） |
| completion tokens | 56,803（~14 tok/轮：平均 4.95 标签 × ~3 tok） |
| 合计 | **1,004,329 tok ≈ 171 tok/轮 × 5882 轮** |
| 语料 | `bench/data/locomo10_enriched.json`（5882/5882 轮有标签，29,402 个标签，均值 5.0/轮） |

量级对比：一次 llm 档全量评测（50 题）≈ 17 次调用；写时增强一次造库 ≈ 4000 次调用
≈ 240 次 judge 评测——一次性成本可接受，但**只有收益成立时才值得付**（本次不成立）。

## 4. 标签质检（随机 20 轮，seed=42）

人工可读样例（完整 20 轮见 `bench/runs/enrich/enrich_full.log` 尾）：

- `D1:3` "I went to a LGBTQ support group yesterday and it was so powerful." →
  `peer gathering, community meeting, identity group, emotional experience, coming together`
- `D8:26` "We also watch "Elf" during the holidays..." →
  `Christmas movie, festive film, holiday tradition, seasonal comedy, family viewing`
- `D10:14` "camping trip ... Perseid meteor shower" →
  `outdoor excursion, stargazing, celestial event, shooting stars, nature bonding`
- 客套轮也被标（"Hey Mel!..." → `greeting, salutation, checking in, well-being, friendly
  encounter`）——NONE 分支基本不触发，泛标签（encouragement/gratitude 类）高频，
  是 cheap 档特征污染的来源之一。

自动质检：泄漏违规（标签含原句内容词）2416/29402 = **8.2%**——逐条抽查全是
单泛词重合（"support group"→`identity group`、"Painting's fun"→`creative hobby`），
**无整短语复读**；prompt 禁令生效，8.2% 是无害的长尾噪声，但这类标签对检索零增量
（词原句已有，只增加文档长度）。

## 5. 蓝方自检逐条

1. **标签泄漏**：prompt 禁复读 + 自动校验 8.2% 单泛词重合（无短语复读），
   见 §4。剩余风险：泛词重合标签不进增量信息，不改变结论。
2. **tags 挂载对 BM25 的干扰**：括号内容被当内容词计分——风险兑现为 cheap 档
   -0.057（特征分布漂移 + 内容分稀释）；bm25 档 +0.005 显示词面通道本身有抗干扰性
   （标签词高 IDF 只在查询词命中标签时才加分）。对策（若转正）：标签迁事件 attr
   专用通道，仅查询扩展使用，不进内容分——spike 范围内不动引擎。
3. **写时成本账**：见 §3，171 tok/轮 × 5882 轮 = 1.00M tok，一次性可接受。
4. **cat5 对抗题影响**：bm25 档 cat5 命中集合纹丝不动（6 题，同一批 dia）——
   无"应弃答题被标签救回"；cheap 档 cat5 命中 8→6（少命中=方向良性）；
   llm 档 cat5 9→10（q156 被救回，该题 gold 有证据，属覆盖率口径内的增益，
   但提示标签文本进了 LLM 判断视野，llm 档 +0.042 纯度存疑，已在 §2 声明）。

## 6. 可复跑

```bash
# 离线增强（缓存续跑；probe 失败即停）
python bench/scripts/enrich_corpus.py            # → bench/data/locomo10_enriched.json
# 对照（bm25/cheap 必跑；--llm 需 API key）
export PATH="$(dirname $(which g++)):$PATH"
python bench/scripts/enrich_ab.py [--llm]        # → bench/runs/enrich/ab_table.md
```

## 7. 结论

写时语义增强（标签挂载进内容通道）**判决不成立**：bm25 Δ=+0.005 ≤ +0.01 预注册线，
cheap Δ=-0.057 负收益；A 类跨改述救回仅 1/50（q27），量级差一个数量级。
改述鸿沟在查询侧的理解能力上，词面标签携带的语义粒度撑不起句级匹配——
与 cheap_rerank_spike"词面特征承载不了句级判断"的判决互相印证。
归档，不并入默认档。
