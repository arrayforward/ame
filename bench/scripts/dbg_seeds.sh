#!/bin/bash
# 编译并运行种子调试工具（用法: bash bench/scripts/dbg_seeds.sh "问题"）
cd /mnt/d/agent/memory/ame
INCS=""
for d in core storage time vector keyword extractor entity graph_writer seed diffusion rerank thinker feedback learning engine bench; do
  INCS="$INCS -I$d/include"
done
LIBS=$(find build -name "libame_*.a" | sort -r | tr '\n' ' ')
INCS="$INCS -Illm/include"
g++ -O2 -std=c++17 $INCS bench/scripts/dbg_seeds.cpp $LIBS $LIBS -o /tmp/dbg_seeds || exit 1
/tmp/dbg_seeds "$1" "$2" "$3"
