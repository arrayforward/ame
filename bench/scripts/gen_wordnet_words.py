#!/usr/bin/env python3
# 从 lexicon JSON 提取全部词条（word 主词 + syn_strong/syn_near/antonyms
# 三流的 key 与值），去重小写，一词一行写提取器大词表。
# 用途：bench --extractor-dict 的 M1 提取器大词表（英文场景；merged.txt 为中文词表）。
# 用法: gen_wordnet_words.py [src.json] [out.txt]（默认 wordnet → wordnet_words.txt）
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / (sys.argv[1] if len(sys.argv) > 1 else "lexicon/lexicon_wordnet.json")
OUT = ROOT / (sys.argv[2] if len(sys.argv) > 2 else "bench/data/wordnet_words.txt")


def main():
    data = json.load(open(SRC, encoding="utf-8"))
    words = set()
    for e in data.get("entries", []):
        w = e.get("word", "").strip().lower()
        if w:
            words.add(w)
        for s in e.get("syn_strong", []) or []:
            s = (s or "").strip().lower()
            if s:
                words.add(s)
        for r in e.get("syn_near", []) or []:
            s = (r.get("word", "") if isinstance(r, dict) else str(r)).strip().lower()
            if s:
                words.add(s)
        for s in e.get("antonyms", []) or []:
            s = (s or "").strip().lower()
            if s:
                words.add(s)
    # 过滤：去空白词；保留词组（AC 支持多词模式）
    words = {w for w in words if w and not any(ch.isspace() for ch in w[0] + w[-1])}
    out = sorted(words)
    OUT.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"{OUT}: {len(out)} 词（entries={len(data.get('entries', []))}）")
    print("示例:", out[:10], "...", out[-5:])


if __name__ == "__main__":
    sys.exit(main())
