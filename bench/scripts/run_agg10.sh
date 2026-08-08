#!/bin/bash
# LME 聚合类 14 题：两阶段（逐项提取+聚合）A/B，基线取 lme_ms12_ircot.ndjson 配对
cd /mnt/d/agent/memory/ame
./build/bench/ame_bench --dataset longmemeval --data bench/data/longmemeval_s_cleaned.json \
  --limit 100 --answer llm --judge llm --concurrency 16 \
  --load-lexicon bench/data/lexicon.json --think on \
  --question-ids bench/data/lme_agg10.json \
  --answer-mode aggregate \
  --cache-dir bench/cache \
  --out bench/runs/lme_agg10_two_stage.json \
  --dump bench/runs/lme_agg10_two_stage.ndjson \
  > bench/runs/lme_agg10_two_stage.log 2>&1
echo "agg10 done rc=$?" >> bench/runs/lme_agg10_two_stage.log
