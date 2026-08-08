#!/bin/bash
# 蒸馏全量收尾：候选池 723,672 词续跑（同一 lexicon_hf.json，断点续跑）
cd /mnt/d/agent/memory/ame
./build/bench/ame_bench --build-lexicon bench/data/vocab_hf.json \
  --lexicon-out bench/data/lexicon_hf.json --lexicon-topn 800000 \
  --concurrency 16 --cache-dir bench/cache \
  > bench/runs/distill_hf_full.log 2>&1
echo "distill_hf_full done rc=$?" >> bench/runs/distill_hf_full.log
