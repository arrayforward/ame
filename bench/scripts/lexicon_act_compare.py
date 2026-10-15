#!/usr/bin/env python3
# 语义 graph 激活工程 A/B 对照：两个 dump NDJSON 的 evidence_dia_ids 覆盖率对照。
# 口径（同 lmem_p0）：每题 coverage = |gold_evidence ∩ evidence_dia_ids| / |gold_evidence|；
# 金标为空跳过；总口径 = 有金标题的覆盖率均值。
# 用法: python bench/scripts/lexicon_act_compare.py A.ndjson B.ndjson
import json
import sys


def load(path):
    rows = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            rows[r["question_id"]] = r
    return rows


def coverage(row):
    gold = row.get("gold_evidence") or []
    ev = row.get("evidence_dia_ids") or []
    if not gold:
        return None
    return len(set(gold) & set(ev)) / len(gold)


def any_hit(row):
    gold = row.get("gold_evidence") or []
    ev = row.get("evidence_dia_ids") or []
    return bool(gold) and bool(set(gold) & set(ev))


def main():
    a_path, b_path = sys.argv[1], sys.argv[2]
    A, B = load(a_path), load(b_path)
    ids = sorted(set(A) | set(B))
    diffs = []
    ca = cb = 0.0
    na = nb = ah_a = ah_b = 0
    for qid in ids:
        ra, rb = A.get(qid), B.get(qid)
        ca_v = coverage(ra) if ra else None
        cb_v = coverage(rb) if rb else None
        if ca_v is not None:
            ca += ca_v
            na += 1
            if any_hit(ra):
                ah_a += 1
        if cb_v is not None:
            cb += cb_v
            nb += 1
            if any_hit(rb):
                ah_b += 1
        if (ca_v or 0) != (cb_v or 0):
            diffs.append((qid, ca_v, cb_v,
                          (rb or {}).get("category"), (rb or {}).get("qtype")))
    print(f"A={a_path}\nB={b_path}")
    print(f"覆盖率均值: A={ca/na:.3f} (n={na}, any-hit {ah_a})  "
          f"B={cb/nb:.3f} (n={nb}, any-hit {ah_b})  Δ={cb/nb-ca/na:+.3f}")
    print(f"逐题不同: {len(diffs)}/{len(ids)}")
    for qid, va, vb, cat, qt in diffs:
        print(f"  {qid} cat{cat} {qt}: {va if va is not None else '-'} -> "
              f"{vb if vb is not None else '-'}")


if __name__ == "__main__":
    main()
