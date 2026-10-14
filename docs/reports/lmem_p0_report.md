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
