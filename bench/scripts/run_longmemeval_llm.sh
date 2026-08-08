#!/bin/bash
# LongMemEval-S 全量评测（rule 摄入零 LLM 成本 + llm 答题 + llm 判分）
# 数据文件预期位置: bench/data/longmemeval_s_cleaned.json（或 longmemeval_s.json）
# 用法: bash bench/scripts/run_longmemeval_llm.sh [数据文件路径]
cd /mnt/d/agent/memory/ame
DATA="${1:-bench/data/longmemeval_s_cleaned.json}"
if [ ! -f "$DATA" ]; then
  DATA="bench/data/longmemeval_s.json"
fi
if [ ! -f "$DATA" ]; then
  echo "数据文件不存在: bench/data/longmemeval_s_cleaned.json 或 longmemeval_s.json" >&2
  exit 1
fi
mkdir -p bench/runs bench/cache
./build/bench/ame_bench --dataset longmemeval --data "$DATA" \
  --extractor rule --answer llm --judge llm --concurrency 8 \
  --cache-dir bench/cache --out bench/runs/longmemeval_llm.json \
  > bench/runs/longmemeval_llm.log 2>&1
echo "longmemeval_llm done rc=$?" >> bench/runs/longmemeval_llm.log
