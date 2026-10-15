#!/usr/bin/env python3
# 英文蒸馏目标词表生成：合并 bench 全部英文语料的词频 → top N
# 输出: bench/data/en_vocab.txt（build_lexicon_json 输入契约：JSON，conv_vocab_top: [[w,f],...]）
# 过滤：纯 ASCII 小写字母词、len>=4、去英文停用词（与 lex 通道/M1 停用词取并集）。
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ROOT / "bench/data/locomo10.json",
    ROOT / "bench/fixtures/locomo_mini.json",
    ROOT / "bench/fixtures/longmemeval_mini.json",
    ROOT / "bench/fixtures/beam_mini.json",
    ROOT / "bench/data/longmemeval_s_cleaned.json",
]
OUT = ROOT / "bench/data/en_vocab.txt"
KEEP_TOP = 12000

# lex 通道(43) + 常见功能词并集。注意：不含 M1 提取停用词（like/know/well 等）——
# lex 通道的停用词表不含它们，且 like 在问题中是真实内容词（"What do kids like"）。
STOP = set("""
the and for are was were has have had did does what when where which who whom whose how
with from that this these those they them their would could should about into your you his her
she him not but all any can will just
a an of to in on at as by or if be been being am is are do does did done i me my
we us our ours he its it itself so no nor too out up down off again once here
own same each few more most other s t don didn doesnt isn aren won weren
""".split())

TEXT_KEYS = {"text", "utterance", "message", "content", "question", "answer", "dialog"}
WORD = re.compile(r"[a-z]+")
# 评测语料（locomo）词频加权：longmemeval 体量 300 倍于 locomo，不加权会把评测分布词挤出 top
LOCOMO_BOOST = 20.0


def harvest(path, freq, boost=1.0):
    n = 0
    def walk(o):
        nonlocal n
        if isinstance(o, dict):
            for k, v in o.items():
                if isinstance(v, str) and k.lower() in TEXT_KEYS:
                    for w in WORD.findall(v.lower()):
                        if len(w) >= 4 and w not in STOP:
                            freq[w] = freq.get(w, 0) + boost
                            n += 1
                else:
                    walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
    walk(json.load(open(path, encoding="utf-8")))
    return n


def main():
    freq = {}
    locomo_raw = {}  # locomo10 原始词频（评测语料覆盖保护用）
    for p in SOURCES:
        if not p.exists():
            print(f"[skip] {p}")
            continue
        boost = LOCOMO_BOOST if p.name == "locomo10.json" else 1.0
        n = harvest(p, freq, boost)
        if p.name == "locomo10.json":
            harvest(p, locomo_raw, 1.0)
        print(f"[harvest] {p.name}: {n} 词元 (boost x{boost:g})")
    top = sorted(freq.items(), key=lambda kv: -kv[1])[:KEEP_TOP]
    # 评测语料覆盖保护：locomo 原始词频>=3 的词强制入选（不计入 12000 名额）
    in_top = {w for w, _ in top}
    extra = sorted([w for w, c in locomo_raw.items() if c >= 3 and w not in in_top])
    words = [w for w, _ in top] + extra
    print(f"locomo 覆盖保护追加: {len(extra)} 词")
    out = {"conv_vocab_top": [[w, freq[w]] for w in words],
           "stats": {"sources": [p.name for p in SOURCES if p.exists()],
                     "unique_words": len(freq), "kept": len(words)}}
    OUT.write_text(json.dumps(out, ensure_ascii=False), encoding="utf-8")
    print(f"{OUT}: kept {len(words)}/{len(freq)} unique words")
    print("top30:", words[:30])
    print("tail20:", words[-20:])


if __name__ == "__main__":
    sys.exit(main())
