#!/bin/bash
# LME 非退化 multi-session 19 题：IRCoT-on 单侧验证（基线取 longmemeval_100.ndjson 配对）
cd /mnt/d/agent/memory/ame
./build/bench/ame_bench --dataset longmemeval --data bench/data/longmemeval_s_cleaned.json \
  --limit 100 --answer llm --judge llm --concurrency 16 \
  --load-lexicon bench/data/lexicon.json --think on --think-mode ircot \
  --question-ids bench/data/lme_ms12.json \
  --cache-dir bench/cache \
  --out bench/runs/lme_ms12_ircot.json \
  --dump bench/runs/lme_ms12_ircot.ndjson \
  > bench/runs/lme_ms12_ircot.log 2>&1
echo "ms12_ircot done rc=$?" >> bench/runs/lme_ms12_ircot.log
