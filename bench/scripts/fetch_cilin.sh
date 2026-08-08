#!/bin/bash
# 第二轮：更多候选源（GitHub jsDelivr + npm tarball）
cd /tmp
URLS=(
  "https://cdn.jsdelivr.net/gh/fighting41love/Cilin_similarity@master/cilin_utf8.txt"
  "https://cdn.jsdelivr.net/gh/ysc/cilin@master/cilin.txt"
  "https://cdn.jsdelivr.net/gh/thunlp/OpenHowNet@master/OpenHowNet-THU/Data/hownet.txt"
  "https://cdn.jsdelivr.net/gh/liuhuanyong/SynonymsExtraction@master/data/synonyms.txt"
)
for u in "${URLS[@]}"; do
  echo "== $u"
  code=$(curl -sS -m 30 -o cilin_try.txt -w "%{http_code}" "$u" 2>&1)
  size=$(stat -c%s cilin_try.txt 2>/dev/null || echo 0)
  echo "   http=$code size=$size"
  if [ "$code" = "200" ] && [ "$size" -gt 10000 ]; then
    head -c 300 cilin_try.txt; echo
    cp cilin_try.txt cilin_raw.txt
    echo "SUCCESS: $u"
    exit 0
  fi
done
echo "-- npm candidates --"
for pkg in chinese-synonyms cilin synonyms; do
  url=$(curl -sS -m 20 "https://registry.npmjs.org/$pkg" | python3 -c "import json,sys
try:
  d=json.load(sys.stdin); print(d['dist-tags']['latest'] and d['versions'][d['dist-tags']['latest']]['dist']['tarball'])
except Exception: print('')" 2>/dev/null)
  echo "pkg=$pkg tarball=$url"
  if [ -n "$url" ]; then
    curl -sS -m 60 -o pkg.tgz "$url" && tar tzf pkg.tgz | head -5
  fi
done
