#!/bin/bash
# LoCoMo 全量 LLM 管线 v2（词法种子+IDF、事件级/出度归一、prompt 优化；提取走缓存）
cd /mnt/d/agent/memory/ame
mkdir -p bench/runs bench/cache
./build/bench/ame_bench --dataset locomo --data bench/data/locomo10.json \
  --extractor llm --answer llm --judge llm --concurrency 16 \
  --cache-dir bench/cache --out bench/runs/locomo_llm_v2.json \
  --dump bench/runs/locomo_llm_v2.ndjson \
  > bench/runs/locomo_llm_v2.log 2>&1
echo "locomo_llm_v2 done rc=$?" >> bench/runs/locomo_llm_v2.log
