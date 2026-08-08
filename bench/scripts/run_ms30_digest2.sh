#!/bin/bash
# LME multi-session 30 题：双通道 digest（摘要指路 + 原文全文）A/B
cd /mnt/d/agent/memory/ame
./build/bench/ame_bench --dataset longmemeval --data bench/data/longmemeval_s_cleaned.json \
  --limit 100 --answer llm --judge llm --concurrency 16 \
  --load-lexicon bench/data/lexicon.json --think on \
  --question-ids bench/data/lme_ms30_ids.json --digest on \
  --cache-dir bench/cache \
  --out bench/runs/lme_ms30_digest2.json \
  --dump bench/runs/lme_ms30_digest2.ndjson \
  > bench/runs/lme_ms30_digest2.log 2>&1
echo "ms30_digest2 done rc=$?" >> bench/runs/lme_ms30_digest2.log
