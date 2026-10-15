# AME 2.0 P0 验收报告：bench 双后端 `--store graph|lmem` 抽样评测（验收项②）

> 设计依据：`docs/design/AME2.0_分层记忆架构设计.md` §10（评测纪律 + P0 验收）；
> 1.x 基线与抽样先例：`docs/reports/locomo_report.md`、`bench/scripts/make_sample50.py`。
> 本报告全部数字可用发布产物一键重生成（命令见 §7，D6 纪律）。

## 1. 本次交付（对应 P0 验收项②）

- **2.0 证据回溯通道（最小扩展）**：`EvidenceItem` 新增 `uint64_t ref_obs`（观测证据=其
  seq；事实证据=`source_obs`），`View::synthesize` 装配时填充；`Mem` 网关新增
  `const Observation* obs(uint64_t seq)` 访问器。bench lmem 后端经
  `ref_obs → Mem::obs(seq) → meta.dia_id` 回溯证据来源，evidence_dia_ids 覆盖率成为
  可算口径。
- **bench 存储抽象**：`bench/include/ame/bench/store.h` 定义窄接口 `IStore`
  （ingest_case + answer_question），`GraphStore`（包 1.x `ingest()/answer()`，
  dia_id 取自事件 attrs）与 `LmemStore`（2.0 `Mem` 网关，owner=conversation_id，
  channel=session_id，dia_id 落观测 meta）两适配器；终排与 1.x 共用同一内容词
  BM25-lite 公式（`content_scores_for_texts`）。
- **CLI 开关**：`--store graph|lmem`（默认 graph，行为与改动前一致，见 §5 回归验证）；
  lmem 下 1.x 专属开关（save/load-graph、load-lexicon、feedback-dir、beam、ircot、
  aggregate、weights）明确报忽略警告。
- **抽样对照脚本**：`bench/scripts/bench_compare_sample.py`——三夹具 + LoCoMo sample50，
  双后端各跑一遍，逐题覆盖率对照表落 `bench/runs/lmem_p0/compare_table.md`；
  `--llm` 走既有磁盘缓存（bench/cache），无 key 自动退化为只比召回覆盖率（judge=N/A）。
- **单元测试**：test_lstore 增 ref_obs 回溯断言与 rebuild 逐比特断言；test_bench 增
  `test_store_adapters`（两适配器同夹具 dia_id 均可回溯、GraphStore 与直接 answer()
  零行为差）。ctest **19/19 全绿**。

## 2. 样本构成（迭代期固定抽样集）

| 配置 | 数据 | 题数 | 说明 |
|---|---|---|---|
| locomo_mini | bench/fixtures/locomo_mini.json | 5 | 三夹具之一（4 题有 dia_id 金标） |
| longmemeval_mini | bench/fixtures/longmemeval_mini.json | 3 | 夹具；session/nugget 口径，无 dia_id 金标 |
| beam_mini | bench/fixtures/beam_mini.json | 4 | 夹具；nugget/tau 口径，无 dia_id 金标 |
| locomo_sample50 | bench/data/locomo10.json + `--question-ids bench/data/locomo_sample50.json` | 50 | 既有 sample50 题单（cat1~5 各 10，**cat2 时序题必入**），48 题有 dia_id 金标 |

- 题单为既有固定产物（sample50 先例，不重新抽样）；完整 LoCoMo 全量 JSON 本地缺失
  （bench/data 仅 locomo10，即 10 个对话全 QA），按纪律**只跑抽样集 + 三夹具**，
  全量留待最终发布口径。
- 双后端同题单、同 extractor（rule）、同缓存目录、同 judge 配置。

## 3. 双后端 evidence_dia_ids 覆盖率对照（硬指标）

覆盖率口径：每题 `|gold_evidence ∩ evidence_dia_ids(top-k, k=10)| / |gold_evidence|`
（Recall@k 集合口径）；any-hit = 金标至少命中一条。

### 3.1 LoCoMo sample50（n=48 有金标）

| category | 题型（1.x 报告口径） | n | 覆盖率均值 graph | 覆盖率均值 lmem | any-hit graph | any-hit lmem |
|---|---|---|---|---|---|---|
| cat1 | 单跳事实 | 10 | 0.125 | 0.125 | 3/10 | 3/10 |
| cat2 | **时序（回归重灾区，必入）** | 10 | **0.700** | **0.700** | 7/10 | 7/10 |
| cat3 | 多跳/推理 | 8 | 0.062 | **0.188** | 1/8 | 2/8 |
| cat4 | 偏好/开放 | 10 | 0.500 | 0.500 | 5/10 | 5/10 |
| cat5 | 对抗（应弃答） | 10 | 0.500 | **0.600** | 5/10 | 6/10 |
| **总体** | | 48 | 0.391 | **0.432** | 21/48 | **23/48** |

**P0 硬指标判定：lmem 覆盖率 ≥ graph 同配置（0.432 ≥ 0.391，any-hit 23 ≥ 21）——达标。**
注意噪声底：1.x graph 引擎本身存在跨 run 波动（同命令两次跑 recall 命中 24 vs 25），
逐题 near-tie 差异 ±1~2 题不具判决意义；均值差 +0.042 方向为 lmem 不劣。

### 3.2 三夹具

| 配置 | 覆盖率 graph | 覆盖率 lmem | 备注 |
|---|---|---|---|
| locomo_mini (n=4) | 1.000 | **0.750** | 退化 1 题，归因见 §5 |
| longmemeval_mini | –（session 口径 recall 2/2 = 2/2；abstain_acc 0.667→1.000） | | |
| beam_mini | –（nugget/tau 双端皆 0，夹具过小仅作冒烟） | | |

### 3.3 逐题差异（sample50，完整表见 bench/runs/lmem_p0/compare_table.md）

- lmem 退化 1 题：`conv-26:q104`（cat4，"What book did Caroline recommend to Melanie?"，
  金标 D7:11）：lmem 在同会话（session_7）取到 D7:8 等邻近轮但金标轮未进 top-10。
- lmem 改善 3 题：`conv-26:q22`（cat3 多跳）、`conv-26:q107`（cat4）、`conv-26:q152`
  （cat5，金标 D2:3，lmem top-3 即命中）。
- 逐题对照表（含 judge 列）：`bench/runs/lmem_p0/compare_table.md`（基线模式）与
  `bench/runs/lmem_p0/compare_table_llm.md`（LLM 模式，正式数字）。

## 4. judge 结果（LLM 答题+判分，走磁盘缓存）

> **环境修复记录（不粉饰）**：首轮 LLM 跑分双后端所有实时请求 transport 失败
> （`http 0`）。根因：1.x `DeepSeekClient::curl_transport` 的 `-w '\n%{http_code}'`
> 在 C 串里是真实换行，WSL/sh 下引号保护换行故历史正常；原生 Windows 经 cmd.exe
> 切分把 `-w` 参数毁掉，状态码永不落盘 → 一律 http 0。历史 28k 缓存条目均 WSL 时代
> 产物。graph 首轮 judge=0.160 实为历史缓存命中残影（8/50 是、42/50 否，20/50 答案
> 是 baseline 回退），lmem 证据文本不同无缓存可命中 → 全回退。**该 bug 对双后端
> 一视同仁，且与本任务无关但阻塞 judge 口径**，已最小修复
> （`llm/src/client.cpp:57`：`-w \"\\n%{http_code}\"`，保持反斜杠-n 字面量由 curl
> 解释；WSL 行为不变），修复后实测 lmem 小夹具 judge=0.800（4/5），实时调用恢复。

| 配置 | judge graph | judge lmem | 覆盖率 graph | 覆盖率 lmem |
|---|---|---|---|---|
| locomo_mini | 0.800 | **0.800** | 1.000 | 0.750 |
| longmemeval_mini | 1.000 | **1.000** | –（session 口径 2/2=2/2） | |
| beam_mini | 0.750 | **0.750** | –（nugget/tau 冒烟） | |
| **locomo_sample50** | 0.520 | **0.560** | 0.391 | **0.432** |

sample50 分 category（judge / 覆盖率）：

| cat | 题型 | judge graph | judge lmem | cov graph | cov lmem |
|---|---|---|---|---|---|
| cat1 | 单跳事实 | 0.2 | 0.3 | 0.125 | 0.125 |
| cat2 | 时序 | 0.7 | **0.7** | 0.700 | 0.700 |
| cat3 | 多跳/推理 | 0.7 | **0.8** | 0.062 | 0.188 |
| cat4 | 偏好/开放 | 0.8 | 0.7（唯一回落项） | 0.500 | 0.500 |
| cat5 | 对抗 | 0.2 | 0.3 | 0.500 | 0.600 |

**P0 judge 判定：lmem judge 不降（0.560 ≥ 0.520；三夹具打平）——达标。**
cat4 judge 单点回落（0.8→0.7）与 §5.2 的取证近失同题源（conv-26:q104），量级 1 题，
不盖过 coverage/judge 双达标；列入 P1 观测。LLM 模式覆盖率与基线模式逐题一致
（取证不受答题方式影响，交叉验证口径自洽）。

## 5. 退化归因（不粉饰）

1. **locomo_mini cat4 退化（q3 "What does Alice probably enjoy doing?"，lmem 零证据弃答）**
   ——根因是 **P0 2.0 召回只有词面倒排精确命中通道**：该题内容词与全部观测零词面
   重叠，lmem 索引召回为空 → 弃答；1.x 靠实体通道圈定 Alice 全部事件 + 软种子相似度
   兜住候选。这是 2.0 P1/P3（语义种子、联想扩散三路召回）的对口改进项，非本次
   存储层 bug。改法方向：索引接入软种子/向量通道，或视图层加 owner 范围兜底层。
2. **conv-26:q104 同会话近失**：金标轮与命中轮同会话、语义相邻但词面重合弱于邻近轮；
   1.x 的 NEXT 链上下文与多通道融合在此起作用。属于检索质量差距，量级 1 题。
3. **cat3 多跳 lmem 反优（0.062→0.188）**：2.0 的 Q→A 打包（user 提问自动附下一条
   assistant 回答）对多跳题的"问-答对"形态更友好；假设待 P3 多跳专项验证。
4. **噪声底声明**：graph 引擎跨 run 存在 ±1~2 题波动（本次复测 recall_hits 24/25），
   故以上逐题归因只在方向性意义上成立，提升声明以发布全量口径为准。

## 6. 实测耗时（时间盒 ≤60min）

| 阶段 | 实测 | 说明 |
|---|---|---|
| 基线模式全套（4 配置 × 2 后端：摄入+召回+本地打分） | **<10s**（sample50：graph 0.7s / lmem 0.1s；合计 0.1 min） | 远低于 60min 时间盒 |
| ctest 19/19 | 9.1s | |
| LLM 模式（--llm，传输修复后正式数字） | **173s（2.9min）**（首轮坏传输白跑 17min 已废弃） | 冷启动：sample50 graph 46.9s / lmem 104.0s（lmem 证据 Q→A 打包略长，prompt 更大）；逐配置 api_calls/cache_hits 见 compare_run_llm2.log |

瓶颈（若扩量）：LLM 答题/判分网络延迟（磁盘缓存可摊薄）；lmem 摄入为内存态逐轮
append，10 对话 4190 轮 0.1s 量级，非瓶颈。

## 7. 复跑命令（D6）

```bash
# 构建 + 单测（Windows/Git Bash：须先固定 mingw PATH，防误载旧 libstdc++-6.dll）
export PATH="$(dirname $(which g++)):$PATH"
cmake --build build-win -j8 && (cd build-win && ctest --output-on-failure)   # 19/19

# 双后端抽样对照（基线模式；--llm 追加 LLM 答题+判分）
export AME_MINGW_BIN="$(dirname $(which g++))"
python bench/scripts/bench_compare_sample.py            # → bench/runs/lmem_p0/
python bench/scripts/bench_compare_sample.py --llm      # judge 口径（走 bench/cache）
```

## 8. 改动文件清单

2.0 侧（内部演进，允许）：
- `memview/include/ame/memview/memview.h` / `memview/src/memview.cpp`：EvidenceItem 增
  `ref_obs` 并在 synthesize 填充（观测=seq，事实=source_obs）。
- `mem/include/ame/mem/mem.h`：网关增 `obs(seq)` 访问器。
- `memindex/include/ame/memindex/memindex.h` / `memindex/src/memindex.cpp`：`tokenize`
  更名 `memindex_tokenize`（与 bench metrics 的 `ame::tokenize` 符号冲突消除，纯改名
  零行为变化）。
- `tests/test_lstore.cpp`：ref_obs 回溯断言 + rebuild 逐比特断言。

1.x 侧（仅一处环境兼容修复，不改任何引擎/metrics 行为）：
- `llm/src/client.cpp`：curl_transport 的 `-w` 格式串修复（原生 Windows 下
  popen→cmd.exe 会按真实换行切分参数导致状态码缺失、全部 http 0；WSL 行为不变）。

bench 侧：
- `bench/include/ame/bench/store.h`（新）：IStore 窄接口 + GraphStore/LmemStore +
  `content_scores_for_texts` 声明。
- `bench/src/store.cpp`（新）：两适配器实现。
- `bench/src/ingest_answer.cpp`：内容词 BM25-lite 终排公式导出为
  `content_scores_for_texts`（content_scores 改为委托，公式与浮点运算序不变）。
- `bench/cli/main.cpp`：`--store graph|lmem`（默认 graph）；lmem 下 1.x 专属开关忽略
  警告；lmem 答题分支（取证/弃答/答案拼接与 metrics 口径对齐，LLM/judge/dump/报表
  下游共用）。
- `bench/CMakeLists.txt`：ame_bench 增加 store.cpp 并链接 ame_mem。
- `tests/test_bench.cpp`：test_store_adapters。
- `bench/scripts/bench_compare_sample.py`（新）：抽样对照脚本。

产物：`bench/runs/lmem_p0/`（graph/、lmem/、graph_llm/、lmem_llm/ 分目录 ndjson+json
+log，对照表 compare_table*.md，本报告）。

## 9. 下一步建议

1. P1：lmem 接入软种子/向量召回通道，修复"零词面重叠 → 零证据"结构性弃答（§5.1）。
2. P1：`--store lmem` 持久化根目录接线（当前内存态，每 case 重建；评测够用，App 需落盘）。
3. P3：cat2 时序题 lmem 与 graph 持平（0.700）但绝对值不高，多分辨率时间桶 + 联想
   扩散落地后按 cat2 12 题专项复测。
4. 全量 LoCoMo JSON 入 bench/data 后，发布口径全量双后端对照（本报告口径不变）。

---

# AME 2.0 P2 验收报告（追加）：兜底层召回 + 来源可靠性闭环 + declare/G1–G3

> 依据：本报告 §5.1（头号实测缺陷归因）+ 设计文档 §6（冲突裁决/来源可靠性）、
> §10 P2 行。全部数字可一键复跑（命令见 P2.7，D6 纪律）。
> 口径变更声明：本次修复了逐题 dump 的 evidence_dia_ids 上限（10→30 条目），
> 理由与影响见 P2.3——**P0 归档的覆盖率数字是截断口径，P2 双后端同用新口径重跑，
> 横向对照公平，纵向与 P0 对比时已标注**。

## P2.1 交付内容

**P2a 兜底层召回通道**（`memview`，不改索引结构）：
- `View::synthesize`：owner 词面命中 < k/2 时启用兜底候选池 = owner 最近 N 条
  （默认 20，可 `set_fallback_pool`）观测，按 (speaker=user 优先, ts 新鲜, seq 决胜)
  排序取 top-k 进打分管线；词面分按命中计（兜底可为 0），可靠性/时间分照常；
- `EvidenceItem` 新增 `via` 区分来源通道 `lex|fallback|fact`（`source_kind` 保持
  原语义：观测 kind / 事实 reality 名，供可靠性闭环定位）；
- **不与正常召回混排竞争**：兜底候选 tier=0，排序键 (tier desc, score desc, seq asc)
  恒居正常候选之后，只作尾部补足；词面命中 ≥ k/2 时零兜底候选，正常路径与 P0
  逐比特一致（排序键退化为 P0 的 (score desc, seq asc)，test_index_rebuild
  逐比特断言全绿验证）；
- 兜底只兜"零证据弃答"：正常召回非空时（tier 地板）也不会挤占任何正常证据的位次。

**P2b 来源可靠性闭环**（`mem` 网关 + `memview` + bench）：
- 新增 `ReliabilityTable`（kind→reliability，默认表与 P0 硬编码逐比特一致；
  读写加锁）：正反馈 +0.02（封顶 1.0）、负 −0.05（封底 0.05，罚>奖防刷）；
- `Mem::feedback(owner, EvidenceItem, positive)`：按证据来源 kind 调整（观测证据
  经 ref_obs 回溯实际 kind，事实证据用 reality 名）；root 非空即持久化
  `shards/reliability.json`（tmp+rename 原子落盘），重启保留；
- 视图层打分改读网关表（替代 P0 硬编码表）；fold 裁决的 reality 级可靠性暂维持
  静态表（任务口径仅要求视图层，列为已知边界）；
- bench：`StoreEvidence` 增 ref_obs/source_kind/via；`LmemStore` 接 1.x 反馈语义
  （judge≥1 → 正、否则负，作用于该题 top-1 证据）；新 CLI `--fb-exp DIR` +
  脚本 `bench/scripts/bench_feedback_exp.py` 实现双 pass 实验（P2.4）。

**P2c declare 原语 + G1–G3 校验门**：
- JNI：`nativeMemDeclare(h, entity, attrsJson, ts)` + `LmemEngine.declare(...)`
  （Java 侧已暴露方法，App 接线留待后续，未动 MemEngine 接口）；
- 写入路径校验（mem 网关内，失败返回错误码、整批不写入）：
  - G1：rememberFact（allocate）要求槽位已存在（declare 创建）；declare 即创建
    语义恒过 G1；
  - G2：attr 非空且 ≤64 字、value ≤256 字（UTF-8 码点计，硬校验）；
  - G3：public 域槽位仅 public owner 可写（个人写 public:* 拦截，个人域不拦）。

## P2.2 验收项对照（硬目标）

| 硬目标 | P0 基线 | P2 实测 | 判定 |
|---|---|---|---|
| locomo_mini q3 不再零证据弃答（lmem 覆盖率回到 1.000） | 0.000（弃答） | **1.000**（2/2 金标入 top-k） | ✅ |
| sample50 lmem 覆盖率 ≥ 0.432 不回退 | 0.432（截断口径） | **0.490**（新口径；graph 同口径 0.448） | ✅ |
| sample50 judge ≥ 0.520 不降 | 0.560 | **0.540**（lmem）；graph 路径未动，P0 归档 0.520 | ✅ |
| cat2 时序保持 0.700 | 0.700 | **0.800**（覆盖率，双后端同值） | ✅ |
| ctest 现有 20 测试全绿 + 新增断言 | 20/20 | **20/20**（test_lstore 138→223 断言，+85） | ✅ |

## P2.3 对照表与逐题 diff

### judge（LLM 模式，走 bench/cache）

| 配置 | graph P0（归档，路径零改动） | lmem P0 | lmem P2 |
|---|---|---|---|
| locomo_mini (n=5) | 0.800 | 0.800 | 见下方环境记录¹ |
| longmemeval_mini | 1.000 | 1.000 | 环境记录¹ |
| beam_mini | 0.750 | 0.750 | 环境记录¹ |
| locomo_sample50 | 0.520 | 0.560 | **0.540** |

¹ **环境记录（不粉饰）**：验收窗口 DeepSeek API 约 50 分钟 503/120s 超时故障
（单条小请求探测可过、bench 批量即超时，判断为上游限流/过载，与本任务代码
无关——失败日志均留痕 `curl: (28) Operation timed out`）。三个 lmem 小夹具的
证据文本因 P2a 兜底尾部发生变化 → 全部冷 prompt → judge 跑分在故障窗口内
transport 全失败（answers 回退 baseline、judge 空响应计 no，judge=0.000 为
**无效跑分，不计入**）。graph 三夹具纯缓存复现 P0 数字（0/6/8/10 api 调用），
佐证 graph 路径零行为变化。sample50 lmem judge=0.540 为故障窗口外的健康
实测（api_calls=5 / cache_hits=99，5 次冷调用全部成功）——**硬目标判定只采用
健康实测与 P0 归档基线，废弃故障窗口跑分**。小夹具 lmem judge 恢复 API 后
按 P2.7 命令一条即补测（预期 q0-q3 证据可直接作答，q4 兜底作答对抗题参考
"无法回答"判 no，量级与 P0 0.800 相当）。

### 覆盖率（基线模式，dump 口径上限 10→30 条目）

| 配置 | graph P0 | lmem P0 | graph P2 | lmem P2 |
|---|---|---|---|---|
| locomo_mini (n=4) | 1.000 | 0.750 | 1.000 | **1.000** |
| locomo_sample50 (n=48) | 0.391 | 0.432 | 0.448 | **0.490** |
| sample50 cat1 | 0.125 | 0.125 | 0.150 | 0.150 |
| sample50 cat2 | 0.700 | 0.700 | 0.800 | 0.800 |
| sample50 cat3 | 0.062 | 0.188 | 0.125 | 0.250 |
| sample50 cat4 | 0.500 | 0.500 | 0.500 | 0.600 |
| sample50 cat5 | 0.500 | 0.600 | 0.600 | 0.600 |

**口径变更归因（不粉饰）**：P0 的逐题 dump 把 evidence_dia_ids 截到 10 条目，
而 lmem 每条证据落 dia_id+"S:会话" 两个条目——top-k=10 条证据只落盘前 5 条，
q3 被救回的金标排在第 6-9 条时被截掉，覆盖率虚低。本次上限放宽到 30 条目
（≈top-k 全量），**对双后端一视同仁**（graph 覆盖率也因此 0.391→0.448、
cat2 0.700→0.800）。这不是引擎变强，是测量修复；两表均已标注。

### 兜底通道触发归因（新增 stderr 计数 `[fallback]`）

- locomo_mini：5/5 题触发兜底（夹具仅 9 条观测，词面命中恒 < k_pool/2=30）；
  q3 全 9 条证据来自兜底（零词面命中），q0-q2 兜底证据恒居词面证据之后
  （tier 地板），覆盖率保持 1.000——**不与正常召回混排竞争的实测验证**；
- locomo_sample50：**0 题触发**（对话体量大 + 分词器不过滤停用词，命中数
  恒 ≥30）。即 sample50 的 lmem 覆盖率变化全部来自口径修复，兜底通道在该
  题单上零副作用。

### 逐题 diff（sample50，P2 vs P2-graph 同口径）

- lmem 优于 graph 2 题：conv-26:q22（cat3）、conv-26:q107（cat3，0→1.000）；
- lmem 无落后于 graph 的题；
- P0 的 conv-26:q104 近失（cat4）在本口径下双端均未命中金标（0.000），
  列 P3 观测。

## P2.4 双 pass 反馈实验（P2b）

`python bench/scripts/bench_feedback_exp.py --quick`（sample50，pass1 全走缓存）：
- pass1 判分 50 题，反馈落盘 50 条（judge≥1 正、否则负，top-1 证据来源 kind，
  同 1.x 语义）；pass2 同目录重开 Mem（replay 恢复观测/事实/可靠性），
  **同题单只跑召回，零 LLM 调用**（本阶段在全部 LLM 阶段之后，不触碰 client）；
- 结果：**up=0 same=27 down=0 nogold=23**（逐题表
  `bench/runs/lmem_p2/feedback_rank_report.md`）；
- **归因（结构性零变化，不粉饰）**：LoCoMo 数据 speaker 为角色名，bench 映射后
  全部观测 kind=assistant——单一来源 kind 的候选集里，kind 级 reliability 是
  全局平移，不改变任何两两相对序；且终排 BM25 不读后端的可靠性分。反馈机制
  本身的"负反馈 rank 降 / 正反馈 rank 升 / 重启保留"由 test_lstore ⑧
  （user/assistant 混合 kind）单测验证。端到端管线（落盘 50 条、
  reliability.json 重启保留、双 pass rank 报告）本次实测打通。

## P2.5 蓝方自检

1. **线程安全（reliability 表读写 vs 单 io 线程）**：ReliabilityTable 内部
   mutex，get/adjust/to_json/load_json 全加锁；JNI 侧本就 per-handle mutex 串行化，
   锁是跨线程兜底，无嵌套锁（feedback→save 顺序取锁，不成环）。
2. **兜底通道与去重的交互**：兜底候选按 seq 跳过词面已命中观测（不重复进池）；
   与 Q→A 打包文本的重复交给原有文本级去重（保序、先高分者留）——单测 ⑦
   断言 ref_obs 两两不同。
3. **reliability 持久化并发/中断**：tmp+rename 原子换文件，中断最多留 .tmp
   残骸、旧表完好；rename 失败退化为直写。内存态（root 空）不写盘。
4. **P0 断言是否被兜底意外改变**：排序键加 tier 主键后，无兜底候选时全序等价
   于 P0 的 (score desc, seq asc)；test_index_rebuild 的逐比特断言
   （drop+rebuild 前后 recall 逐字段一致）与零副作用断言（replay dump 逐比特）
   原样全绿；sample50 上兜底 0 触发，行为与 P0 一致。
5. **bench 双 pass 缓存污染**：pass2 只调 recall，代码路径零 LLM client 触碰；
   且 pass1 的答题/判分 prompt 与 compare 共用磁盘缓存，pass2 无任何新 prompt。
   实验前脚本清空 fb-exp 分片目录（shutil.rmtree），防重复摄入污染 pass2 的
   replay。
6. **G1 对既有调用方的影响**：rememberFact 新增"槽位须先 declare"后，存量
   单测 4 处调用点已补 declare 适配（语义即设计 §7：开槽是 declare 的动词）；
   bench/迁移路径只用 remember（观测层），不受影响。

## P2.6 耗时（时间盒 ≤60min）

| 阶段 | 实测 |
|---|---|
| ctest 20/20 | 10.3s |
| 基线模式全套（4 配置 × 2 后端） | <10s |
| LLM 模式全套 | 本轮无完整健康计时（API 故障窗口）；P0 同口径 173s；健康缓存热 run sample50 双端合计 <20s（日志 llm api/cache 行） |
| 双 pass 反馈实验（sample50） | 6.3min（pass1 全缓存；含 lmem 落盘+replay） |

**环境记录（不粉饰）**：验收期间 DeepSeek API 出现约 50 分钟 503/120s 超时
故障，三个 lmem 小夹具的首轮 judge 跑分全部 transport 失败（answers 回退
baseline、judge 空响应计 no）——非引擎退化，已在日志留痕
（`curl: (28) Operation timed out`）。按时间盒纪律停线不无限重试；硬目标
判定不依赖小夹具 judge（见 P2.3 环境记录¹），恢复后一条命令即可补测。

## P2.7 复跑命令（D6）

```bash
export PATH="$(dirname $(which g++)):$PATH"
cmake --build build-win -j8 && (cd build-win && ctest --output-on-failure)   # 20/20
export AME_MINGW_BIN="$(dirname $(which g++))"
python bench/scripts/bench_compare_sample.py            # 基线口径 → runs/lmem_p0/
python bench/scripts/bench_compare_sample.py --llm      # judge 口径（走 bench/cache）
python bench/scripts/bench_feedback_exp.py --quick      # P2b 双 pass（→ runs/lmem_p2/）
```

## P2.8 改动文件清单

2.0 侧：
- `memview/include/ame/memview/memview.h` / `memview/src/memview.cpp`：
  ReliabilityTable（P2b）、EvidenceItem.via、tier 排序、兜底候选池（P2a）、
  打分读可靠性表。
- `mem/include/ame/mem/mem.h` / `mem/src/mem.cpp`：feedback + reliability.json
  持久化（tmp+rename）、G1–G3 校验门、root_ 成员。
- `memfact/include/ame/memfact/memfact.h` / `memfact/src/memfact.cpp`：
  `has_slot`（G1 判定）。
- `android/ame_jni/mem_jni.cpp`：nativeMemDeclare。
- `android/app/src/main/java/com/ame/memory/LmemEngine.java`：declare 暴露。

bench 侧：
- `bench/include/ame/bench/store.h` / `bench/src/store.cpp`：StoreEvidence 增
  ref_obs/source_kind/via；now 兜底改观测层扫描（持久化恢复后口径一致）；
  `[fallback]` 触发计数（stderr）。
- `bench/cli/main.cpp`：`--fb-exp` 双 pass 块（feedback→落盘→pass2 只召回→
  rank 对比 + ndjson fb_rank 字段）、dump 证据上限 10→30、1.x 反馈/M14 守卫
  加 store=="graph"。
- `bench/scripts/bench_feedback_exp.py`（新）：双 pass 实验脚本。
- `tests/test_lstore.cpp`：⑦兜底召回（零重叠返回 fallback 证据且 user 优先/
  新鲜度排序、正常路径无 fallback、部分命中时 fallback 居正常候选之后、ref_obs
  不重复）⑧可靠性闭环（负反馈 rank 降/正反馈 rank 升/封底封顶/reliability.json
  落盘/重启保留）⑨G1–G3 门（declare 权威 latest、allocate 先开槽、attr/value
  长度硬校验含 64/256 边界、public 拦截）；4 处存量 rememberFact 补 declare
  （G1 适配）。合计 138→223 断言（+85）。

产物：`bench/runs/lmem_p2/`（feedback_rank_report.md、fbexp_data/ 分片、日志）。

## P2.9 遗留与下一步

1. fold 三序裁决的 reality 级可靠性仍为静态表（任务口径外，feedback 目前只
   作用于视图层打分）——多来源（提取器/LLM 插件）接入后需打通 fold 路径。
2. 单一 kind 数据集上 kind 级反馈天然无排序差（P2.4 归因）；P2 设计中的
   "LLM 提取插件"上线后来源 kind 多样化，反馈闭环的排序效应才可在 bench 观测。
3. App 侧 LmemEngine.declare 接线（声明入口 UI/orchestrator 调用点）。
