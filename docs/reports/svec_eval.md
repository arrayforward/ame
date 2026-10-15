# 句向量（sentence embedding）离线评估报告

**分支**：`svecl-line`（基于 dev-2.0 `cf9f726`）｜ **性质**：纯测量，不集成，C++ 引擎零改动
**问题**：端侧小模型（bge-small 级）作 cheap 档追加特征，能把 0.54~0.57 带抬到多少，值不值 ~100MB APK？
**判决（预注册口径）**：复刻实测 **0.578**（cheap+svec vs cheap 0.464，Δ=+0.114）→ 落在 **灰区上沿（0.57~0.60）**；加性投影端到端 0.65~0.69 触成立线但依赖假设（§5）。**不按"成立"直接推进集成**；要钉死需引擎真接入复跑（唯一出灰区路径）。

---

## 1. 模型获取结果

hf-mirror.com 全量成功（hf 直连未试，按任务书镜像优先）：

| 模型 | 文件 | 大小 | 用途 |
|---|---|---|---|
| `Xenova/bge-small-en-v1.5` | `onnx/model_quantized.onnx` | 34.0 MB | **主测量**（int8，onnxruntime CPU） |
| 同上 | `onnx/model.onnx`（fp32） | 133.1 MB | APK 体积账（首次下载中断，重试成功） |
| 同上 | `tokenizer.json` | 0.71 MB | 最小 BertTokenizer（WordPiece，自实现，无 transformers 依赖） |
| `Xenova/bge-small-zh-v1.5` | `onnx/model_quantized.onnx` + tokenizer | 24.0 MB | **仅端侧尺寸参照**，未在英文语料上评估 |

- 存放 `bench/models/`（已加入 .gitignore，权重不入 git）；embed 缓存 `bench/runs/svec/embed_cache/`（文本 sha1 键，`bench/runs/` 已忽略）。
- bge 口径：384 维，last_hidden_state 按 attention mask 均值池化 + L2 归一；**查询侧加官方检索指令** `"Represent this sentence for searching relevant passages: "`，证据侧不加。
- LoCoMo 为英文语料：en 模型是主测量对象；zh 模型按任务书只作尺寸参照（24MB int8 ≈ en 的 70%）。

## 2. 方法

脚本 `bench/scripts/svec_eval.py`（零 transformers 依赖，onnxruntime + numpy + 自实现 WordPiece 分词器）：

1. **embed**：sample50 每题问题 + 候选池（每题 60~67 条，去重后 358 条证据 + 50 题）全部磁盘缓存（sha1(model|kind|text)）。
2. **svec_cos 特征**：问题向量·证据向量余弦（向量已归一 = 点积），**题内 minmax**——与 C++ `cheap_score_pool` 的池相对归一口径逐一对齐（初版脚本训练用原始余弦、复刻用 minmax 的口径不一 bug 已修复并重跑，§7 蓝方 #2）。
3. **重训**：复用 `cheap_rerank_train.py` 框架（同 m12_cal 标签、同 42 种子 5-fold GroupKFold、同纯 Python 逻辑回归），12 特征 = 11 旧 + `svec_cos`；基线 11 特征在同进程同折复算（复现 T2 的 0.710/0.514，精确一致）。
4. **端到端重判 = Python 复刻重放**：`replay_cheap()` 逐行复刻 `ingest_answer.cpp` 的终排（默认 `final_mix=关` → key=cs 降序 stable）+ `cheap_score_pool`（minmax/秩/stable 排序/sigmoid）+ `apply_selection`（置顶 top-10）。**零 LLM、确定性**。
5. **Kendall tau**：B 类 6 题（q10/q101/q154/q155/q34/q50），svec 排序 vs m12_cal LLM 全池教师排序（日志"选中"序 = LLM 响应序）。

## 3. 测量结果

### 3.1 交叉验证（同折同标签，seed=42；种子敏感性 seed 1/7/99）

| 模型 | AUC | 正例 top-10 召回 |
|---|---|---|
| cheap v2（11 特征，复算） | 0.710 | 0.514 |
| **cheap+svec（12 特征）** | **0.782** | **0.607** |
| svec 种子 1 / 7 / 99 | 0.786 / 0.779 / 0.783 | 0.616 / 0.603 / 0.604 |

- ΔAUC = **+0.072**（±0.005 跨种子），Δ召回 = **+0.093**——imitation 指标大幅抬升，且远 ent_overlap 级（+0.011）不是一个量级。
- 学出的 `svec_cos` 权重 **+2.07**，为全模型最大正权重；负权重簇与 v2 同向（ent_overlap −0.90、ctx −0.87、time −1.32）。

### 3.2 端到端复刻重放（sample50，覆盖率 = |金标∩top10|/|金标| 宏平均）

| 配置 | 覆盖率 | any-hit | 校准 |
|---|---|---|---|
| BM25 原序 | 0.443 | 24/48 | = 引擎 off 实测 0.443/0.448 ✓ |
| cheap v2 模型 | 0.464 | 24/48 | 引擎 cheap 实测 0.536/0.573（差距来源见下） |
| **cheap+svec 模型** | **0.578** | **30/48** | — |
| 裸 svec_cos 排序（无其它特征） | 0.359 | — | **低于 BM25** |

- **Δ = +0.114**：救回 7 题（q15 0→0.25、q19 0→0.5、q101/q103/q108/q153/q157 0→1.0），丢失 1 题（q32 0.25→0）。
- 复刻 vs 引擎的系统性差距（0.464 vs 0.536~0.573）：pools50 是**标准池**，不含实体通道并入候选——14 题金标根本不在标准池（q1/q2/q3/q18/q22/q24/q27/q34/q42/q50/q64/q106/q156/q160），引擎端靠 entity-pass 0.5 救回其中一部分。**该差距对两模型同向、与 svec 无关**，A/B 差值（+0.114）干净。
- 关键判读：**句向量不能单独承载排序**（裸 cos 0.359 < BM25 0.443）；价值全部来自"与词面/图特征互补的线性组合"。

### 3.3 Kendall tau：svec vs LLM 全池教师（B 类 6 题）

| qid | τ(svec, LLM 选中集) | τ(svec, LLM 全池) | τ(bm25, LLM 全池) | τ(fusion, LLM 全池) | 金标 svec 秩 / LLM 秩 |
|---|---|---|---|---|---|
| q10 | +0.000 | +0.806 | +0.825 | +0.760 | 6 / 1 |
| q101 | +0.429 | +0.946 | +0.840 | +0.716 | 4 / 2 |
| q154 | −0.067 | +0.898 | +0.876 | +0.829 | 14 / 1 |
| q155 | −0.111 | +0.957 | +0.882 | +0.888 | 7 / 1 |
| q34 | +0.143 | +0.834 | +0.747 | +0.768 | 金标不在池 |
| q50 | n/a | n/a | n/a | n/a | 教师当轮无有效输出降级 |

- **全池 tau 平均 +0.888 是假象成分居多**：bm25 词面序的全池 tau 也有 +0.834——全池一致性由"明显无关的尾部"主导，任何不荒谬的排序都能拿到。
- **决定性的选中集 tau ≈ 0**（−0.11~+0.43，均 ≈ +0.08，近随机）：在 LLM 实际选中的 top-10 内部，svec 的相对排序与教师基本不一致；金标在 svec 序中排 4~14（教师 1~2）。
- **结论：bge-small 离 LLM 教师的 top-10 判别距离 ≈ 近随机**（教师噪声未分离，q50 教师无输出，见蓝方 #6）。它抬 cheap 靠的是"把 BM25 压不到中段的语义相关候选提前"，而非复刻教师排序。

## 4. 端侧集成账（若集成）

| 项 | 数值（PC 实测） | 说明 |
|---|---|---|
| APK 增量（fp32） | **133.8 MB**（133.1 模型 + 0.71 词表） | "~100MB" 说法的实际量级 |
| APK 增量（int8） | **34.7 MB** | 集成建议形态；zh 参照 24.7 MB |
| 每查询延迟 | **~21 ms**（单问题 embed，batch=1）+ 60×384 维点积（<1 ms）+ 向量读盘 ~92 KB | 证据向量在**写时** embed 落盘，查询侧只做 1 次 embed |
| 写时一次性成本 | 358 条 6.5 s（≈55 条/s，batch 64）；整 case 419 事件 ≈ 8 s | 随摄入异步摊销 |
| 运行时内存 | ≈ 模型体积（int8 34 MB / fp32 133 MB）+ onnxruntime 工作集 | 未精测 RSS，按权重体积给下界 |

## 5. 判决（预注册三档）

- **复刻实测覆盖率 0.578 vs cheap 现状带 0.54~0.57** → 落在 **灰区上沿（0.57~0.60）**。
- 加性投影（引擎带 + Δ_sim）= **0.650~0.687 ≥ 0.60** 触成立线——但依赖"Δ 可加"假设（entity-pass 并入候选上 svec 的表现未实测，`is_entity` 训练恒 0、模型权重 0.0，方向中性偏乐观）。
- **判定：灰区上沿，不按"成立"推进集成。** 与 N1 线"r1 触线 r2 未触不翻案"同一纪律：单口径触线不换结论。
- **出灰区所需（按优先级）**：
  1. **引擎真接入 svec_cos 复跑 sample50 端到端**（写时 embed 入 attrs / 向量侧车文件 + `cheap_score_pool` 加一特征）——唯一消除加性假设的路径，也是 int8 34.7MB 值不值的最终判据；
  2. 若 1 不可行：换蒸馏目标（用 m12 选中做对比学习直接优化池内排序，代替"12 特征之一"的线性用法）再测 CV；
  3. 更大端侧模型（bge-base 级 ~440MB fp32）超出 APK 预算，不推荐。
- **APK 问题的直接回答**：fp32 133.8MB **不值**（灰区增益配不上）；int8 34.7MB 是"若接受灰区定位可进"的形态——但要钉死先走上面第 1 条。

## 6. 与 N1 线（LLM 档）的关系

- cheap+svec 复刻 0.578 已**超过 m2（LLM 全池单机制 0.526）**，距冻结 LLM 档 0.63~0.67 仍差 ~0.06~0.09；句向量不是 LLM 档的平价替代，是兜底的强化。
- tau 测量（§3.3）量化了差距的结构：小模型在教师 top-10 内部排序近随机，差距是**判别性**的，不是数据量级的。

## 7. 蓝方自检

1. **泄漏**：svec 是独立第三方模型（BAAI/Xenova），与标签来源（m12 LLM 选中日志）零同源；`svec_cos` 只用问题文本+证据文本；缓存键 sha1(model|kind|text) 不含标签。**无泄漏**。
2. **训练/服务口径 bug（已修复并披露）**：初版训练用原始余弦、复刻用题内 minmax（train/serve 不一致），当时 CV AUC 0.724、复刻 Δ 仅 +0.02；统一为题内 minmax 后 AUC 0.782、Δ +0.114。报告数字全部以修复后为准；该经历也验证了"特征口径必须与集成口径一致"的集成纪律。
3. **分词器正确性**：自实现最小 BertTokenizer 经两轮校验——初版 normalize 误丢标点（"hello world!" 错分成 hello+##world），修复后 [7592, 2088, 999] 与 BERT 官方词表逐一相符；缓存随修复整体作废重算（键不含分词器版本，靠人工清库，已写进 §2 备注）。
4. **中英适用性**：LoCoMo 英文，en 模型（30.5k 英文 WordPiece）为主测量；zh 模型（24MB int8）按任务书只作端侧尺寸参照，未在英文上跑分——若未来语料转中文需重测 zh 模型质量。
5. **复刻保真度**：BM25 重放 0.443 = 引擎 off 实测（0.443/0.448）逐位一致；基线复刻 0.464 低于引擎 cheap 带（0.536~0.573），差距结构已知（标准池无实体通道并入候选，14 题金标不在池，对两模型同向）——**A/B 差值 +0.114 不受该差距污染**，但"端到端绝对值"是投影而非实测，已入判决条件。
6. **教师噪声未分离**：tau 用 m12_cal 单轮选中序（q50 当轮教师无有效输出，n=5）；LLM 选中集内部序的轮间稳定性未测，选中集 tau（≈0）的解释力受此限制——全池 tau 高不是能力证据（bm25 同样高，§3.3）。
7. **多重比较**：本任务只追加 1 个特征、无扫参；CV 用固定 42 种子主数字 + 3 个种子敏感性（§3.1），未按最好种子挑数。
8. **q32 丢失**（0.25→0）已列 §3.2；救回 7 题逐题列出，无隐藏翻转。
9. **纪律**：C++ 零改动（复刻为纯 Python）；模型权重与 embed 缓存均在 gitignore 路径；时间盒内完成（下载为大头，镜像单文件重试 1 次成功）。

## 8. 改动清单

- `bench/scripts/svec_eval.py`（新）：获取校验 + 最小分词器 + onnxruntime embed（磁盘缓存）+ svec_cos 重训/同折 CV + 端到端复刻重放 + B 类 tau + 体积/延迟账，单脚本可复跑（缓存命中后秒级）。
- `.gitignore`：+ `bench/models/`。
- 运行产物（不入库）：`bench/runs/svec/`（`cheap_model_svec.json`、`sim_per_question.json`、`embed_cache/`）；`bench/models/`（两模型 ONNX + 词表）。
- 外部输入（主工作树产物，只读引用）：`ame/bench/runs/mechanisms/{pools50.ndjson, m12_cal.log, cheap_model_v2.json}`。

复跑命令：

```bash
python bench/scripts/svec_eval.py \
  --pools ../ame/bench/runs/mechanisms/pools50.ndjson \
  --log   ../ame/bench/runs/mechanisms/m12_cal.log \
  --baseline-model ../ame/bench/runs/mechanisms/cheap_model_v2.json
```
