#!/bin/bash
# 勘察 funNLP 源目录各文件编码与格式样例
cd /mnt/d/agent/memory/dict/funNLP-master/funNLP-master/data
FILES=(
  THUOCL_diming/THUOCL_diming.txt
  THUOCL_food/THUOCL_food.txt
  THUOCL_chengyu/THUOCL_chengyu.txt
  THUOCL_IT/THUOCL_IT.txt
  THUOCL_lishimingren/THUOCL_lishimingren.txt
  professions/professions.txt
  input/同义词.txt
  input/30wdict_utf8.txt
  input/dict.txt
  input/httpcws_dict.txt
  input/词.txt
  input/输入法.txt
  input/fingerDic.txt
  input/out.txt
  input/输入2.TXT
  input/30wChinsesSeqDic_clean.txt
)
for f in "${FILES[@]}"; do
  if [ ! -f "$f" ]; then echo "== $f [缺失]"; continue; fi
  echo "== $f ($(stat -c%s "$f") bytes)"
  head -c 400 "$f" | od -c | head -2
  head -3 "$f" | cat -v
  echo
done
