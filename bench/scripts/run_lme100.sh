#!/bin/bash
# LongMemEval-S 前 100 题评测（当前最优检索配置，后台跑 + 进度日志）
cd /mnt/d/agent/memory/ame
mkdir -p bench/runs
./build/bench/ame_bench --dataset longmemeval --data bench/data/longmemeval_s_cleaned.json \
  --limit 100 --answer llm --judge llm --concurrency 16 \
  --load-lexicon bench/data/lexicon.json --think on \
  --cache-dir bench/cache \
  --out bench/runs/longmemeval_100.json \
  --dump bench/runs/longmemeval_100.ndjson \
  > bench/runs/longmemeval_100.log 2>&1
echo "lme100 done rc=$?" >> bench/runs/longmemeval_100.log
