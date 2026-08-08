#!/bin/bash
cd /mnt/d/agent/memory/ame
INCS=""
for d in core storage time vector keyword extractor entity graph_writer seed diffusion rerank thinker feedback learning engine bench llm; do
  INCS="$INCS -I$d/include"
done
LIBS=$(find build -name "libame_*.a" | sort -r | tr '\n' ' ')
g++ -O0 -g -std=c++17 $INCS bench/scripts/ircot_dbg.cpp $LIBS $LIBS -o /tmp/ircot_dbg || exit 1
/tmp/ircot_dbg
