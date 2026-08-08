#!/bin/bash
# LoCoMo 全量 LLM 管线评测（后台运行；缓存断点续跑）
# 用法: bash bench/scripts/run_locomo_llm.sh
cd /mnt/d/agent/memory/ame
mkdir -p bench/runs bench/cache
./build/bench/ame_bench --dataset locomo --data bench/data/locomo10.json \
  --extractor llm --answer llm --judge llm --concurrency 8 \
  --cache-dir bench/cache --out bench/runs/locomo_llm.json \
  > bench/runs/locomo_llm.log 2>&1
echo "locomo_llm done rc=$?" >> bench/runs/locomo_llm.log
