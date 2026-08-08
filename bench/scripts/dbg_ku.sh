#!/bin/bash
cd /mnt/d/agent/memory/ame
INCS=""
for d in core storage time vector keyword extractor entity graph_writer seed diffusion rerank thinker feedback learning engine bench llm; do
  INCS="$INCS -I$d/include"
done
LIBS=$(find build -name "libame_*.a" | sort -r | tr '\n' ' ')
g++ -O2 -std=c++17 $INCS bench/scripts/dbg_ku.cpp $LIBS $LIBS -o /tmp/dbg_ku || exit 1
/tmp/dbg_ku
