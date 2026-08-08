#!/bin/bash
cd /mnt/d/agent/memory/ame
g++ -O2 -std=c++17 -Iextractor/include -Icore/include -Itime/include -Istorage/include \
  bench/scripts/bench_ac.cpp \
  build/extractor/libame_extractor.a build/time/libame_time.a \
  build/storage/libame_storage.a build/core/libame_core.a \
  build/storage/libame_storage.a build/core/libame_core.a \
  -o /tmp/bench_ac || exit 1
/tmp/bench_ac
