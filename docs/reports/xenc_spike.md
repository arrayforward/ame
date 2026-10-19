# cross-encoder 精排 spike 报告（--rerank xenc，句级语义谱系第 2 档）

**分支**：`dev-2.0`｜ **目标**：检验 cross-encoder 能否吃掉 cheap(0.557)→llm(0.672) 差距
**判决（预注册）**：覆盖率两次均值 ≥0.62 推荐中间档成立 / 0.58-0.62 灰区 / <0.58 证伪；
tau vs 教师 ≥+0.35（svec 参照 +0.216）→ 蒸馏立项依据成立。

---

## 1. 实现

- **模型**：bge-reranker-base（Xenova ONNX **int8 279MB**，hf-mirror 全量下载；
  首次 75MB 截断→断点续传不被镜像支持→全量重下成功，载入/打分冒烟通过：
  相关对 -5.82 vs 离题对 -10.18）。
- **分词器**：bge-reranker 系用 **Unigram（sentencepiece 风格）** 而非 WordPiece
  ——新实现 `MiniUnigramTokenizer`（Metaspace 预切分 ▁ + Viterbi 最大分；
  normalizer 为 Precompiled charsmap，本实现对英文语料近似恒等——近似声明见
  蓝方 #1）。`xenc_scorer.py --pool` 协议与 svec 完全一致（popen 每题一次）。
- **pair 构造**（位置偏置口径披露）：`[CLS] q [SEP] d [SEP]`，token_type
  q=0/d=1（BGE reranker 训练惯例）；max_seq 512（q~15+d~60 token，绰绰有余）。
- **得分**：[CLS] logits（pair 分类头）；pair 级磁盘缓存
  `bench/models/cache/xenc/{sha1(模型|q|d)}.npy`。
- **C++**：`--rerank xenc` 六档并列（bm25|cheap|svec|formula|xenc|llm）；
  得分直接定序（判别式打分非特征合成）；guard/entity-pass 横切照旧；
  **scorer 失败 → 降级 BM25 原序**（单测 test_xenc_rerank 实测）。

## 2. 对照表与判决（冻结配置 entity-pass 0.5 + guard 0.2，sample50）

| 臂 | 覆盖率 | any-hit | judge |
|---|---|---|---|
| bm25 | 0.464 | 25/48 | – |
| cheap | 0.557 | 29/48 | – |
| svec | 0.578 | 31/48 | 0.160（旧口径） |
| **xenc（本 spike）** | **0.354**（两次逐位一致，确定性） | 17/48 | **0.720**（跟随测） |
| llm | 0.672 | 37/48 | 0.72~0.74 |

- **判决：覆盖率 0.354 < 0.58 → 证伪归档**（预注册 <0.58 档）。裸 cross-encoder
  定序大幅劣于 bm25（-0.11）——与 WMD/svec/formula 同向：**任何"单模型打分
  直接定序"的形态在 LoCoMo 都打不过"引擎结构特征 + 学习权重"**（cheap 0.557）。
  距 llm 档差距 +0.318 的目标完全未达成。
- **tau vs 教师选中集：+0.164**（37 题，svec 同口径 +0.216）——**未上台阶**
  （<+0.35）：裸 bge-reranker 与 LLM 教师的排序一致性还不如双编码器余弦，
  **蒸馏立项依据不成立**（至少在"裸模型零样本"形态；教师偏离 topical 偏好
  改述金标，cross-encoder 的 MS MARCO 式相关性训练同样偏好 topical——
  两个"教师"结构性错配）。
- **judge 跟随测 0.720**：覆盖率之外的有趣面——cross-encoder 选出的 top-10
  topical 集中度高，进了 gold 的题作答质量好（与 llm 档 judge 同带）。但
  覆盖率证伪是硬指标，此面只作记录。

## 3. 成本/体积账

| 项 | 实测 |
|---|---|
| 模型体积（int8） | **279MB**（vs svec 的 34MB——8 倍，端侧账完全不可行） |
| 每题 scorer 延迟 | 冷 中位 2963ms/p90 3098ms；热 中位 2623ms（**每题一次 python+onnx 重载 ~2s 主导**，纯推理仅 ~300-500ms/60 对） |
| 50 题合计 | 冷 147s / 热 132s（外挂形态账；常驻进程形态 ≈ 50×0.4s = 20s） |
| 查询 LLM | 零 |

## 4. 蓝方自检

1. **分词器近似**：Unigram 的 Precompiled normalizer 以恒等近似（英文语料
   影响小）；冒烟验证跨句词 id 一致、相关对分数明显高于离题对。
2. **max_seq 截断**：512 上限 vs 实测 pair ~80 token，无截断发生（确认即可）。
3. **batch 内存**：32 对/batch × ≤512 token × 768 hidden——int8 下 <100MB
   峰值，无压力。
4. **与双编码器得分相关性**：tau 对比已量化——cross-encoder 与教师一致性
   +0.164 < svec +0.216，**非互补而是同向且更弱**（同为 topical 偏好系）。
5. **位置偏置**：q/d 顺序与 token_type 按 BGE 训练惯例（披露）；未做顺序
   翻转消融（时间盒，若立项蒸馏时补）。
6. **缓存键**：sha1(模型目录名|q|d)，跨运行复用（热跑 60 对中 ~36-60 命中，
   剩余来自运行间 uid 噪声的池文本微差）。
7. **下载纪律执行**：首次截断 75MB → 断点续传不支持（镜像 416/拒绝）→
   全量重下 279MB 成功；未触发 MiniLM 降级（主模型最终可得，用时 ~10min）。

## 5. 产物

- 代码：`bench/scripts/xenc_scorer.py`（含 MiniUnigramTokenizer）、
  `bench/scripts/xenc_tau.py`、`bench/include/ame/bench/bench.h`、
  `bench/cli/main.cpp`、`bench/src/ingest_answer.cpp`（run_xenc_scorer +
  xenc 分支）、`tests/test_bench.cpp`（test_xenc_rerank，**159/159 全绿**）。
- 运行产物（不入库）：`bench/runs/svec_eng/xenc_{r1,r2,judge}.*`、
  `bench/runs/wmd/xenc_tau.json`、`bench/models/bge-reranker-base/`（gitignore）。
- 复跑：`ame_bench ... --rerank xenc [--xenc-scorer PATH]`
