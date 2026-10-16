# 商业对标报告：Mem0/Zep 行业记忆产品 × AME（研究期 · 抽样口径）

**分支**：`dev-2.0`｜ **阶段定位**：研究期，全部评测用 sample50 抽样口径；**本期不含全量评测，也不留全量计划项**。
**指标**：LoCoMo sample50 金标覆盖率 |gold∩检索 top-10|/|gold| 宏平均（n=48）+ any-hit，与 AME 四档完全同口径。
**基础设施**：`bench/venv-commercial/`（隔离 venv，tuna 镜像）+ `bench/scripts/openai_shim.py`（OpenAI 兼容双端 shim：chat→DeepSeek 磁盘缓存转发，embeddings→本地 bge-small int8——DeepSeek 无 embedding API 的本地替代，配置即 `commercial_mem0.py` 的 from_config 块）。

---

## 1. 对照总表（sample50 同口径）

| 臂 | 覆盖率 | any-hit | 经济性档 | 写入成本 | 查询延迟 | LLM 后端 |
|---|---|---|---|---|---|---|
| AME bm25 | 0.464 | 25/48 | **零 LLM** | 零 | 毫秒级 | – |
| AME cheap | 0.557 | 29/48 | **零 LLM** | 零 | 毫秒级 | – |
| AME svec | 0.578 | 31/48 | **零 LLM**（写时 embed ~8s/整案） | ~21ms/题 PC | – |
| 自参照 dense-turn（bge-small） | 0.347 | 19/48 | 零 LLM | 零 | 毫秒级 | – |
| AME llm | 0.672 | 37/48 | **查询时 LLM**（写时零成本） | 零 | ~4k tok/题 | DeepSeek |
| **Mem0 OSS 2.2.1** | **0.703** | 37/48 | **写时 LLM**（查询无 LLM，见 §4） | **47min/838 次调用/整案** | **中位 2.25s/题** | DeepSeek + 本地 bge |
| Zep/Graphiti | 不可比 | – | 写时+查询时 LLM | – | – | 官方 GPT（见 §5 降级） |

Mem0 行实测细节：419 轮写入产出 **409 条事实级记忆**（LLM 提取+去重合并）；
严格口径=宽松口径=**0.703**（无跨轮合并记忆挂多 dia_id，见蓝方 #1）。

## 2. 判读：AME 在哪个经济性档位打赢谁

1. **零-LLM 档：AME 全面胜**。AME cheap 0.557 / svec 0.578 vs 行业裸 RAG 自参照
   dense-turn 0.347（+0.21~+0.23）；vs 词面参照 bm25-turn 0.479 亦胜。零推理
   成本下 AME 的记忆层（融合特征+图扩散+实体通道+学习重排）显著强于
   "切块+向量"的默认形态——与 industry_baseline.md 结论互证。
2. **写时-LLM 档：Mem0 以重成本换 +0.03~+0.13**。Mem0 0.703 是全部臂最高，
   但它花 47 分钟/838 次 LLM 调用完成写入（每案），查询中位 2.25s/题；同档
   位对比：AME svec 0.578（写时仅 ~8s 本地 embed、查询 21ms）覆盖率差
   -0.125，**但写入成本差 ~350 倍、查询延迟差 ~100 倍**；AME llm 0.672 与
   Mem0 差 -0.031，而 AME llm **写时零成本**（无提取管线，直接索引原文）。
   覆盖率每 +0.01 的边际成本：Mem0 ≫ AME llm ≫ AME svec。
3. **查询时-LLM 档**：AME llm 0.672 + 作答质量修复后 judge 0.64（answer_quality.md）
   是"查询时 LLM"形态里成本最优的（每题一次全池打分 ~4k tok，写时免费）。
4. **产品级结论（研究期）**：若目标"覆盖率尽量高且不在乎写入成本"→ Mem0 形态
   最优（0.703）；若目标"零/低 LLM 成本的记忆层"→ AME cheap/svec 显著强于
   同成本的一切行业参照；AME 的空档在"写时-LLM 提取"（Mem0 靠它拿到
   +0.125），这是后续可评估引入的形态——**本期只记录事实，不改 AME**。

## 3. Mem0 臂实现与口径映射（复现路径）

- 写入：`m.add([{user 轮文本}], user_id="conv-26", metadata={"dia_id": D#})`
  （mem0 2.2.1 强制 user_id；内部 LLM 事实提取，~2 次调用/轮）。
- 查询：`m.search(question, limit=10, filters={user_id})` → top-10 记忆，取
  `metadata.dia_id` 并集计覆盖率。
- **粒度差异披露**：Mem0 记忆是**事实级**（409 条 distilled facts）非轮级
  （AME/参照臂是 419 轮取 10）。top-10 记忆 ≠ top-10 轮：单轮可产出多条记忆，
  记忆的信息密度高于轮——该差异对 Mem0 **有利**（10 条事实覆盖的 dia 多样性
  通常 ≥ 10 条原文），如实标注不修正。
- 复跑：`bench/venv-commercial/Scripts/python.exe bench/scripts/commercial_mem0.py`
  （全量重写 qdrant）；`--search-only` 只查不重写。

## 4. 成本与延迟账（实测）

| 项 | Mem0 臂 | 备注 |
|---|---|---|
| 写入 | 47min/整案（419 轮，838 次 LLM 调用） | ~6.5s/轮（DeepSeek 思考型输出） |
| 写入 token | 估 ~0.5-0.7M input + ~84k output | 按调用次数×典型 prompt 长度估 |
| 写入美元 | **~$0.3-0.5/整案**（deepseek-chat 官方价 $0.27/M in、$1.10/M out 假设） | 价格随官方变动，以 token 数为准 |
| 每题分摊 | ~$0.01（写） | 查询无 LLM |
| 查询延迟 | 中位 2254ms / p90 2605ms | **无 LLM 调用的实测**（证据：缓存文件数恰=2×419，search 零 chat 调用）——2.25s 为本链路程实测开销 |
| 缓存复跑 | shim 磁盘缓存按请求体 sha1 | 但见蓝方 #4：mem0 增量提取不可缓存复现 |

AME 对照：svec 档写时 ~8s 本地 embed/整案 + 21ms/题；llm 档写时零 + ~4k tok/题。

## 5. 降级项：Zep/Graphiti（2 次尝试，如实降级）

- **尝试 1**：`pip install graphiti-core`（tuna）成功，`Graphiti(uri, user,
  password, llm_client, embedder)` 可导入——但构造强依赖 **Neo4j 服务**
  （Bolt 连接），本机无实例。
- **尝试 2**：评估替代（Zep Cloud 需外部 API key，无；嵌⼊式/内存图存储
  graphiti-core 不支持）→ 不满足运行条件。
- **降级**：官方公开数字 + 口径分析（研究期不硬磕重型外部服务）：
  - Zep 官方 LoCoMo 80.32%@189ms（单检索档）~ 83%（高配置档，[getzep.com/research](https://www.getzep.com/research/)）；
    Mem0 官方 LoCoMo 91.6（2026-04 算法，[mem0 官方对比](https://mem0.ai/library/agent-memory/zep-vs-mem0-which-ai-memory-layer-should-you-choose)）。
  - **口径不可比面**：①官方是全量 LoCoMo 1,540 题、端到端 accuracy（含其
    官方 GPT 答题管线和 harness），我们是 sample50 子集的**检索覆盖率**；
    ②LLM 后端不同（GPT vs 我们 DeepSeek）；③我们实测的 Mem0 OSS 0.703 也是
    sample50+DeepSeek+覆盖率口径，与官方 91.6 不构成矛盾也不并列比较
    ——三个口径差异各自披露，数字只作量级参照。
- Letta：未尝试（时间盒让给 Mem0 实跑与 Zep 降级分析；Letta 需常驻 server，
  形态与"记忆库"对标弱）。

## 6. 蓝方自检

1. **dia_id 双口径**：宽松 = top-10 记忆 metadata.dia_id（列表则并集，含跨轮
   合并记忆挂的全部 dia_id）；严格 = 仅单 dia_id 记忆。**实测两口径同值
   0.703**——写入过程中未产生挂多 dia_id 的合并记忆（409 条均单挂），故
   合并污染未发生；粒度差异（事实级≠轮级）在 §3 披露且不修正。
2. **各家 top-k 语义差异**：Mem0 limit=10 是 10 条事实（浓缩），AME/参照臂
   是 10 轮（原文）——信息密度不对等，对 Mem0 有利方向已标注；AME 侧
   entity-pass 并入候选使其 top-10 偶尔 >10 个不同 dia，两向差异不系统性
   偏向一方，披露为止。
3. **LLM 后端不可比面**：Mem0 臂与我们全部 AME 臂同用 DeepSeek（同 key 只读）
   + 本地 bge 嵌入——**臂间可比**；Zep 官方数字用 GPT-4o 后端 → 只在 §5 作
   量级参照，不进对照表判读。
4. **缓存复跑**：shim 按请求体 sha1 磁盘缓存；实测发现 **mem0 增量提取的
   prompt 依赖当前记忆库状态**（事实更新检查携带已有记忆），逐轮 add 的
   顺序重放无法命中缓存（第二轮近乎全量重算，838 新文件）——这是 Mem0
   "写时 LLM"形态的成本结构事实，如实披露；AME 各臂无此问题（检索确定性
   + 缓存键纯文本哈希）。
5. **环境隔离**：venv（gitignore）、PostHog 遥测警告出现但未拦截（mem0
   默认开 telemetry，未配置 key 故无上报出口）；spaCy/fastembed 可选件缺失
   只关停了 mem0 的 BM25 关键词通道（其语义通道不受影响），如实记录。
6. **研究期纪律**：本期全部结论基于 sample50（n=48）；不在本报告列全量
   评测计划项；所有数字可用 §3 命令复现（LLM 缓存命中后写入重放仍需
   ~45min，查询秒级）。

## 7. 产物清单

- `bench/scripts/openai_shim.py`（新）：OpenAI 兼容 shim（chat 缓存转发 +
  本地 bge 嵌入 + dimensions 零填充兼容）。
- `bench/scripts/commercial_mem0.py`（新）：Mem0 臂（含 `--search-only`）。
- `.gitignore`：+ `bench/venv-commercial/`。
- 运行产物（不入库）：`bench/runs/commercial/`（mem0_results.json、
  llm_cache/、qdrant/、日志）。
