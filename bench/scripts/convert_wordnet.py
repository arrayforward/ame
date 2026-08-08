#!/usr/bin/env python3
# WordNet(wordnet-db npm 包 dict/data.*) → AME lexicon.json 词条转换器（离线，零 LLM）
# 输入: package/dict/data.{noun,verb,adj,adv}（WordNet 3.1 文本格式）
# 输出: bench/data/lexicon_wordnet.json（entries: [{word, synonyms, antonyms, related}]）
# 词行格式: offset lex_filenum ss_type w_cnt (word lex_id)* p_cnt (symbol offset pos src tgt)* | gloss
import json, sys, collections

DICT_DIR = "/tmp/package/dict"
OUT = "/mnt/d/agent/memory/ame/bench/data/lexicon_wordnet.json"

synsets = {}   # offset -> words
pointers = collections.defaultdict(list)  # offset -> [(symbol, target_offset)]

for pos in ("noun", "verb", "adj", "adv"):
    for line in open(f"{DICT_DIR}/data.{pos}", encoding="utf-8", errors="replace"):
        if line.startswith(" "):
            continue  # license 头
        bar = line.find("|")
        if bar < 0:
            continue
        parts = line[:bar].split()
        if len(parts) < 4:
            continue
        offset = int(parts[0])
        w_cnt = int(parts[3], 16)
        words = []
        idx = 4
        for _ in range(w_cnt):
            words.append(parts[idx].lower().replace("_", " "))
            idx += 2  # 跳过 lex_id
        synsets[offset] = words
        if idx < len(parts):
            p_cnt = int(parts[idx])
            idx += 1
            for _ in range(p_cnt):
                if idx + 3 < len(parts):
                    sym, tgt = parts[idx], int(parts[idx + 1])
                    pointers[offset].append((sym, tgt))
                idx += 4

entries = []
for offset, words in synsets.items():
    ants, rels = [], []
    for sym, tgt in pointers.get(offset, []):
        if sym == "!" and tgt in synsets:
            ants.extend(synsets[tgt])
        elif sym in ("@", "&") and tgt in synsets:  # 上位/相似（跨 synset → 近义）
            for w in synsets[tgt]:
                rels.append((w, 0.9))
    for w in words:
        # 分流：synset 内部成员=同义(syn_strong)，跨 synset 上位/相似=近义(syn_near)
        syns = [x for x in words if x != w]
        entries.append({
            "word": w,
            "syn_strong": syns[:8],
            "syn_near": [{"word": r, "score": s} for r, s in rels[:8]],
            "antonyms": list(dict.fromkeys(ants))[:4],
        })

json.dump({"version": "wordnet3.1-split", "entries": entries},
          open(OUT, "w", encoding="utf-8"), ensure_ascii=False)
strong_pairs = sum(len(e["syn_strong"]) for e in entries)
near_pairs = sum(len(e["syn_near"]) for e in entries)
ant_pairs = sum(len(e["antonyms"]) for e in entries)
print(f"synsets={len(synsets)} entries={len(entries)} strong={strong_pairs} "
      f"near={near_pairs} ant={ant_pairs}")
print("out:", OUT)

# 与 LoCoMo 词表的覆盖率报告
v = json.load(open("/mnt/d/agent/memory/ame/bench/data/vocab_freq.json"))
vocab = {w for w, _ in v["conv_vocab_top"]} | {w for w, _ in v["question_vocab_top"]}
lex_words = {e["word"] for e in entries}
cov = vocab & lex_words
print(f"vocab={len(vocab)} covered={len(cov)} coverage={len(cov)/len(vocab):.1%}")
missing = sorted(vocab - lex_words, key=lambda w: -dict(v["conv_vocab_top"]).get(w, 0))[:20]
print("未覆盖高频词样例:", missing)
