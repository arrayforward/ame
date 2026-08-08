#!/bin/bash
# LME multi-session 3 题（seed=42）：IRCoT 模式 A/B
cd /mnt/d/agent/memory/ame
./build/bench/ame_bench --dataset longmemeval --data bench/data/longmemeval_s_cleaned.json \
  --limit 100 --answer llm --judge llm --concurrency 16 \
  --load-lexicon bench/data/lexicon.json --think on --think-mode ircot \
  --question-ids bench/data/lme_ms3_ids.json \
  --cache-dir bench/cache \
  --out bench/runs/lme_ms3_ircot.json \
  --dump bench/runs/lme_ms3_ircot.ndjson \
  > bench/runs/lme_ms3_ircot.log 2>&1
echo "ms3_ircot done rc=$?" >> bench/runs/lme_ms3_ircot.log
