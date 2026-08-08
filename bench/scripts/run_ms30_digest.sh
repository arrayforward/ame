#!/bin/bash
# LME multi-session 30 题 digest A/B（后台长任务）
cd /mnt/d/agent/memory/ame
./build/bench/ame_bench --dataset longmemeval --data bench/data/longmemeval_s_cleaned.json \
  --limit 100 --answer llm --judge llm --concurrency 16 \
  --load-lexicon bench/data/lexicon.json --think on \
  --question-ids bench/data/lme_ms30_ids.json --digest on \
  --cache-dir bench/cache \
  --out bench/runs/lme_ms30_digest.json \
  --dump bench/runs/lme_ms30_digest.ndjson \
  > bench/runs/lme_ms30_digest.log 2>&1
echo "ms30_digest done rc=$?" >> bench/runs/lme_ms30_digest.log
