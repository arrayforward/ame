# archive_retired/ — 3.0 已裁实验脚本留存

**状态**：dev-3.0 七刀裁剪中被移入本目录集中留存，**不删除**——docs/reports/ 各报告
（svec_eval.md / formula_rerank.md / xenc_spike.md / wmd_spike.md / answer_quality.md /
industry_baseline.md / commercial_bench.md）引用这些脚本保证复跑性。

| 脚本 | 原属 | 裁剪刀 | 留存原因 |
|---|---|---|---|
| svec_scorer.py | svec 档外挂打分器（句向量） | 刀5 | svec_eval.md §9 引擎复跑节引用 |
| xenc_scorer.py / xenc_tau.py | cross-encoder 精排档 | 刀5 | xenc_spike.md 复跑命令引用 |
| formula_fit.py | 公式档权重拟合 | 刀5 | formula_rerank.md §5 复跑引用 |
| wmd_eval.py | WMD 词组合 spike | 刀5 | wmd_spike.md §5 复跑引用 |
| enrich_corpus.py / enrich_ab.py | 写时增强 spike | 刀5 一并归档 | 报告引用 |
| oracle_ceiling.cpp | oracle 工具（pools50/E1-E3） | 刀6 | 多个报告的训练数据来源工具 |
| bench_ac.cpp / dbg_*.cpp / ircot_dbg.cpp / prof_lme.cpp | 一次性调试/剖析工具 | 刀6 | 历史诊断存档 |

**注意**：C++ 一次性工具（oracle_ceiling/dbg_* 等）在 3.0 源码树上可能因被裁模块
（mem*/feedback/learning/thinker/lexicon 灌库）不再编译——它们的历史产物
（pools50.ndjson / m12_cal.log 等）已固化在 bench/runs/，报告数字不依赖重新编译。
svec_eval.py / ref_rag_baseline.py / commercial_mem0.py / openai_shim.py 等**未裁**
脚本仍在 ../ 下原路径。
