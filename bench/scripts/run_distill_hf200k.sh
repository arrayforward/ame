#!/bin/bash
# top 200000 高频词蒸馏（后台长任务，断点可续；并发 16 已试跑验证稳定）
cd /mnt/d/agent/memory/ame
./build/bench/ame_bench --build-lexicon bench/data/vocab_hf.json \
  --lexicon-out bench/data/lexicon_hf.json --lexicon-topn 200000 \
  --concurrency 16 --cache-dir bench/cache \
  > bench/runs/distill_hf200k.log 2>&1
echo "distill_hf200k done rc=$?" >> bench/runs/distill_hf200k.log
