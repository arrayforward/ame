# 基准评测报告索引

> **3.0 口径注记**：本目录为研究期归档。其中词库召回/svec/xenc/formula/WMD/
> 写时增强/think/lmem 等机制已在 dev-3.0 裁撤（依据即这些报告），
> 现行保留资产与基线以仓库 README"3.0 简化版"节为准。

AME 联想记忆引擎在两个长期记忆基准上的评测报告与失败分析归档。
所有数字均来自本地 `bench/runs/` 下的评测产物（JSON 汇总 + NDJSON 逐题 dump +
运行日志），原始 dump 不入库（.gitignore 排除），本目录只提交报告文档。

| 报告 | 内容 |
|---|---|
| [locomo_report.md](locomo_report.md) | LoCoMo 1986 题 v1/v2/v3 全量对比、分 category 指标、cat2 时序题失败分析（双根因+消融归因）、cat5 弃答观察、修复 A/B |
| [longmemeval_report.md](longmemeval_report.md) | LongMemEval-S 前 100 与全量 500 对比、分题型指标、偏好题失败分析与修复 A/B、multi-session 聚合三轮假设存档 |

## 评测方法论

### 指标口径

- **judge（LLM 判分）**：DeepSeek 对"系统回答 vs 参考答案"判 0/1（语义正确性，
  不是字符串匹配），报表取均值。是本工程的主指标。
- **token F1（离线代理）**：回答与参考答案的词元 F1（LoCoMo 官方口径之一），
  零 API 可跑；对"正确但措辞不同"的回答偏低，仅作粗筛。
- **recall@k（证据召回）**：retrieval 侧指标，与答题解耦。
  LoCoMo：top-k 证据的 dia_id 与 gold evidence 交集非空记命中；
  LongMemEval：top-k 证据的 session_id 命中 answer_session_ids 记命中。
- **abstain_acc（弃答准确率）**：在**应弃答**（对抗/无答案）题目上正确输出
  NO INFORMATION 的比例；在可答题目上报表另以 abstain 率呈现于逐题 dump。
  注意与"弃答率"区分：前者越高越好（对抗题），后者过高说明误弃答。

### 离线代理 vs LLM 口径

指标栈分两层：`IJudge`/`ILlmClient` 接口可整体切换离线代理（nugget 词重叠、
token F1）与真实 LLM judge。离线代理用于免 API 的回归冒烟；所有结论性数字
均以 LLM judge 口径为准。LoCoMo 的 category 编号按不透明整数处理（cat1~5，
不臆测官方题型名），其中 cat5 经配置判为对抗（应弃答）题。

### 缓存与复跑

LLM 调用全量磁盘缓存（prompt+model 哈希，`bench/cache/`）：同 prompt 复跑零新调用。
典型复跑方式：

```bash
# 同参数第二遍（缓存重放，验证确定性/零扣费）
./build/bench/ame_bench --dataset longmemeval --data bench/data/longmemeval_s_cleaned.json \
  --limit 500 --answer llm --judge llm --concurrency 16 --cache-dir bench/cache \
  --load-lexicon bench/data/lexicon.json --think on \
  --out report.json --dump results.ndjson
# 固定小样本（--question-ids 会跳过无匹配 case 的摄入，分钟级出结果）
./build/bench/ame_bench ... --question-ids bench/data/lme_pref30.json
```

### 逐题失败分析方法

每轮评测产出 NDJSON 逐题 dump（question/answer/abstained/judge/evidence_top3/
evidence_dia_ids/gold_evidence），失败分析在 dump 上离线进行（零 API）：
按 检索未召回 / 召回但误弃答 / 召回答了但答错 / judge 误判 四类归因，
先定量后动手；修复用小样本 A/B 验证（修复前后同题集 judge/recall 对比），
有效才放大到全量。
