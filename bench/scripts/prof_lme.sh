#!/bin/bash
# 编译并运行 LongMemEval 性能剖析工具
cd /mnt/d/agent/memory/ame
INCS=""
for d in core storage time vector keyword extractor entity graph_writer seed diffusion rerank thinker feedback learning engine bench; do
  INCS="$INCS -I$d/include"
done
LIBS=$(find build -name "libame_*.a" | sort -r | tr '\n' ' ')
g++ -O2 -std=c++17 $INCS bench/scripts/prof_lme.cpp $LIBS $LIBS -o /tmp/prof_lme || exit 1
/tmp/prof_lme
