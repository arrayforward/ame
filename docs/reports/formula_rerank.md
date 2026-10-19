# 路由打分公式重排（--rerank formula）报告

**分支**：`dev-2.0`｜ **定位**：cheap 档的上位替代候选（零 LLM、公式可解释）——**验收判决：不成立（拟合版 0.448 < 0.57 预注册线），cheap 兜底地位不动**；档保留为可选实验档（与 svec 档同地位，不入默认）。
**指标**：LoCoMo sample50 金标覆盖率（|gold∩top-10|/|gold|，n=48）+ any-hit；冻结横切配置 entity-pass 0.5 + guard 0.2。

---

## 1. 实现

新档 `--rerank formula`（C++：bench.h 配置 / main.cpp CLI / ingest_answer.cpp
`formula_score_pool`；单测 test_formula_rerank）：

1. **窗口 M**：`--formula-window`（默认 120）→ `cfg.k_pool = M`（60→120 扩池，
   实测入池 121~125 条/题）；
2. **七语义关系特征**（全部 Engine 图结构提取，每候选 ~1ms/题）：
   | 特征 | 定义 | 先验权重 |
   |---|---|---|
   | bm25 | 内容词 BM25-lite 分 / 池内 max | 0.9 |
   | involves | 查询实体节点→事件 INVOLVES BFS 最小跳数倒数（≤2 跳，无连接=0） | 0.7 |
   | related | 查询词↔事件关键词：直连=1.0；否则 RELATED 六类边 边权×传导系数 最大值 | 0.9 |
   | next | 与最近 BM25 命中（cs>0）事件的叙事序（ts 位次）距离倒数 | 0.5 |
   | cooccur | 查询词↔事件关键词共现结晶边数 log1p | 0.6 |
   | time | 时间词题的时间窗相容：窗=(提问日-180d, 提问日]，窗内 +1/窗外 -1；非时间题=0 不激活 | 0.6 |
   | fusion | 1.x 融合分池内 minmax | 0.4 |
   | opposite | 查询词↔事件关键词经 REL_OPPOSITE：减分（负值） | -0.5 |
3. **公式**：`score = Σ wᵢ·fᵢ`，权重 CLI 可配 `--formula-w "w0,...,w7"`；
4. **可解释性产物**：`--formula-dump` 落每候选逐特征贡献 ndjson（诊断向）；
5. **权重离线拟合**：`bench/scripts/formula_fit.py`（坐标下降，题级 5-fold
   GroupKFold，AUC 目标，权重保持 8 维线性可读）。

## 2. 对照表（预注册验收：≥0.60 成立 / 0.57-0.60 灰区 / <0.57 不成立）

| 臂 | 覆盖率 | any-hit | 备注 |
|---|---|---|---|
| bm25 | 0.464 | 25/48 | 词面基准 |
| cheap | 0.557 | 29/48 | **兜底地位不动** |
| svec | 0.578 | 31/48 | cheap+svec_cos |
| llm | 0.672 | 37/48 | 查询时 LLM |
| **formula 先验版** | **0.391**（×2 复跑逐位一致） | 21/48 | 确定性零噪声 |
| **formula 拟合版** | **0.448**（重放 0.411） | 25/48 | 拟合权重见 §3 |

**判决：两版均 < 0.57 → formula 档不成立。** 先验版大幅劣于 bm25（0.391
vs 0.464）说明先验权重方向性错误（图关系加分项在这个 benchmark 上不是正贡献）；
拟合修正后仍只有 0.448——七关系线性公式在 M=120 池上拿不到 top-10 命中。
judge 跟随测：formula 档 + answer/judge llm = **0.680**（formula_judge.out，
覆盖率被检索上限压住，judge 由作答管线撑起）。

## 3. 七关系贡献表（数据判决：拟合权重 + 消融）

**拟合权重**（全量拟合，train AUC 0.795 / 5-fold held-out AUC 0.755，gap +0.04
过拟合面可接受）：`bm25 2.0, involves -0.5, related -0.2, next 2.0, cooccur 0.3,
time 0.6, fusion -1.0, opposite -0.5`（与先验的差距：involves/related/fusion
**变号**、bm25/next 拉满、cooccur 减半）。

**消融（拟合权重上置 0 一个特征看重放覆盖率 Δ）**：

| 特征 | Δ 覆盖率 | 判决 |
|---|---|---|
| bm25 | **-0.318** | 绝对主力（预期内） |
| fusion | -0.042 | 次贡献（预期内） |
| involves | -0.021 | 微贡献 |
| related | -0.021 | 微贡献 |
| cooccur | -0.021 | 微贡献 |
| next | ±0.000 | **本 benchmark 无贡献**（权重 2.0 拉满但只重排 bm25 高分的邻近项，不改变命中集） |
| time | ±0.000 | **无贡献**（窗谓词激活面窄，且窗外 -1 在对抗题上反而伤） |
| opposite | ±0.000 | **无贡献**（反义边命中面≈0） |

**结论**：七关系里 3 个（next/time/opposite）在本 benchmark 上无贡献，
3 个（involves/related/cooccur）合计贡献 +0.02 量级，拟合后前两者变号——
**"图语义关系线性加分"假设在 LoCoMo 检索上基本不成立**；这与 svec 档结论
同向（句向量裸排序 0.359 也远不及管线组合）：AME 检索力的来源是
**学习到的非线性特征交互 + 池相对归一**（cheap 档 AUC 0.71/0.782），不是
任何一种单语义的线性叠加。

## 4. 成本与蓝方自检

- **特征计算成本**：120 池 × 7 特征 ≈ **0.9~1.3ms/题**（日志 `[formula]`
  feat_ms 实测）——零 LLM、毫秒级，成本面无问题（问题不在成本在效果）。
- **时间窗误杀面**：f_time 设计为减分非剔除；实测弃答率 0.78~0.80 与 bm25
  基准带一致，窗外 -1 未造成弃答异常；但其覆盖率贡献为 0 → 特征保留但
  判"无贡献"。
- **拟合过拟合**：题级 GroupKFold held-out AUC 0.755（train 0.795，gap +0.04）
  ——拟合本身可泛化；失败原因是**目标错位**：AUC 0.755 在 120 池上仍不够
  top-10 命中（正例 1~2 个/题），AUC≠召回。
- **先验 vs 拟合差距解读**：involves/related 先验 +0.7/+0.9 → 拟合 -0.5/-0.2
  ——图关系加分把"与查询实体/词图邻近"的事件提前，但 LLM 教师选中的金标
  恰是**改述/弱图连接**轮（与 cheap 档 ent_overlap 学出负权重同因）；
  fusion 变号同理（图扩散高分≠教师偏好）。
- **先验版=确定性**（两次复跑逐位一致，零 LLM 零随机），无噪声带问题；
  拟合版端到端 0.448 vs 离线重放 0.411 的小差来自 entity-pass 并入候选的
  运行间边界翻转，均已披露。
- **纪律**：formula 不成立 → 不设为任何默认/兜底（cheap 不动）；档代码与
  测试保留（可选实验 + 可解释性 dump 基础设施可复用）；单测 test_bench
  **155/155**。

## 5. 产物

- 代码：`bench/include/ame/bench/bench.h`、`bench/cli/main.cpp`、
  `bench/src/ingest_answer.cpp`、`tests/test_bench.cpp`（test_formula_rerank）。
- 拟合脚本：`bench/scripts/formula_fit.py`（可复跑，缓存=引擎 dump 复用）。
- 运行产物（不入库）：`bench/runs/svec_eng/formula_*.{ndjson,out,log}`、
  `formula_dump.ndjson`（6080 条逐候选特征）、`formula_fit.json`。
- 复跑：
  `ame_bench ... --rerank formula [--formula-window 120] [--formula-w "…"]
   [--formula-dump path]`
