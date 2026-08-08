#!/bin/bash
# 两阶段聚合降本组合：本地预过滤(N=20) + 批量提取(5条/次) A/B（c 组）
cd /mnt/d/agent/memory/ame
./build/bench/ame_bench --dataset longmemeval --data bench/data/longmemeval_s_cleaned.json \
  --limit 100 --answer llm --judge llm --concurrency 16 \
  --load-lexicon bench/data/lexicon.json --think on \
  --question-ids bench/data/lme_agg10.json \
  --answer-mode aggregate --agg-prefilter-n 20 --agg-batch 5 \
  --cache-dir bench/cache \
  --out bench/runs/lme_agg10_batch.json \
  --dump bench/runs/lme_agg10_batch.ndjson \
  > bench/runs/lme_agg10_batch.log 2>&1
echo "agg10_batch done rc=$?" >> bench/runs/lme_agg10_batch.log
