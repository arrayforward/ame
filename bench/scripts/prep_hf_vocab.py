#!/usr/bin/env python3
# 高频词蒸馏目标清单生成：合并 segmentation+domain 词频 → top N 中文词，过滤已覆盖
# 输出: bench/data/vocab_hf.json（build_lexicon_json 直接可读的词频格式）
import json, glob, os, re, sys

BASE = "/mnt/d/agent/memory/ame/bench/data"
TOP_N = int(sys.argv[1]) if len(sys.argv) > 1 else 5000

freq = {}
# segmentation/merged.txt: word\tfreq\tsources
for ln in open(f"{BASE}/lexicon_src/segmentation/merged.txt", encoding="utf-8"):
    p = ln.rstrip("\n").split("\t")
    if len(p) >= 2 and p[1].isdigit():
        freq[p[0]] = max(freq.get(p[0], 0), int(p[1]))
# domain/*.txt: word\tfreq\tcategory
for path in glob.glob(f"{BASE}/lexicon_src/domain/*.txt"):
    for ln in open(path, encoding="utf-8"):
        p = ln.rstrip("\n").split("\t")
        if len(p) >= 2 and p[1].isdigit():
            freq[p[0]] = max(freq.get(p[0], 0), int(p[1]))
print("全库词条:", len(freq))

# 已覆盖词集合：三本中文词典 + 英文词典的主词
covered = set()
for name in ("lexicon_zh.json", "lexicon_zh2.json", "lexicon_wordnet.json",
             "lexicon.json", "lexicon_gap.json"):
    p = f"{BASE}/{name}"
    if not os.path.exists(p):
        continue
    d = json.load(open(p, encoding="utf-8"))
    for e in d.get("entries", []):
        if e.get("word"):
            covered.add(e["word"])

CJK = re.compile(r"[\u4e00-\u9fff]")
cands = []
skip_cov = skip_bad = 0
for w, f in freq.items():
    if w in covered:
        skip_cov += 1
        continue
    # 过滤：纯数字/单字/标点/无中文字符/过短
    if (len(w) < 2 or w.isdigit() or not CJK.search(w) or
            all(not c.isalnum() and not CJK.search(c) for c in w)):
        skip_bad += 1
        continue
    cands.append((w, f))
cands.sort(key=lambda x: -x[1])
target = cands[:TOP_N]
json.dump({"conv_vocab_top": [[w, f] for w, f in target], "question_vocab_top": []},
          open(f"{BASE}/vocab_hf.json", "w", encoding="utf-8"), ensure_ascii=False)
print(f"已覆盖跳过: {skip_cov}  格式过滤: {skip_bad}  候选: {len(cands)}  目标: {len(target)}")
print("top10:", [w for w, _ in target[:10]])
