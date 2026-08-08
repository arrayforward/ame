#!/bin/bash
# 重启 LoCoMo LLM 全量跑（缓存续跑）。精确匹配二进制路径，避免误杀外层 shell。
pkill -f "build/bench/ame_bench" 2>/dev/null
sleep 1
cd /mnt/d/agent/memory/ame
setsid nohup bash bench/scripts/run_locomo_llm.sh < /dev/null > /dev/null 2>&1 &
sleep 2
ps aux | grep "build/bench/ame_bench" | grep -v grep | wc -l
