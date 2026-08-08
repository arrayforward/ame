#!/bin/bash
# 两阶段聚合修正版（保留思考）5 题验证
cd /mnt/d/agent/memory/ame
./build/bench/ame_bench --dataset longmemeval --data bench/data/longmemeval_s_cleaned.json \
  --limit 100 --answer llm --judge llm --concurrency 16 \
  --load-lexicon bench/data/lexicon.json --think on \
  --question-ids bench/data/lme_agg5.json \
  --answer-mode aggregate \
  --cache-dir bench/cache \
  --out bench/runs/lme_agg5_two_stage.json \
  --dump bench/runs/lme_agg5_two_stage.ndjson \
  > bench/runs/lme_agg5_two_stage.log 2>&1
echo "agg5 done rc=$?" >> bench/runs/lme_agg5_two_stage.log
