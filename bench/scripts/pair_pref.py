#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 偏好题修复 A/B 对比：before = longmemeval_full.ndjson 中 single-session-preference 30 题，
# after = lme_pref30_fixed2.ndjson（角色感知第一人称 + 通道弃权回退 + 内容词终排）。
import json, os

RUNS = os.path.join(os.path.dirname(__file__), "..", "runs")

def load(name):
    out = {}
    with open(os.path.join(RUNS, name), encoding="utf-8") as f:
        for line in f:
            if line.strip():
                r = json.loads(line)
                out[r["question_id"]] = r
    return out

old = load("longmemeval_full.ndjson")
new = load("lme_pref30_fixed2.ndjson")
pref_ids = [q for q, r in old.items() if r.get("qtype") == "single-session-preference"]

def stat(recs, ids):
    j = sum(1 for q in ids if recs[q].get("judge") == 1)
    ab = sum(1 for q in ids if recs[q].get("abstained"))
    return j, ab

jo, ao = stat(old, pref_ids)
jn, an = stat(new, pref_ids)
print(f"before: judge {jo}/30  abstain {ao}/30")
print(f"after : judge {jn}/30  abstain {an}/30")
print("\n翻转明细：")
for q in pref_ids:
    o, n = old[q], new.get(q)
    if not n:
        continue
    mark = ""
    if o["judge"] == 0 and n["judge"] == 1:
        mark = "↑ 0→1"
    elif o["judge"] == 1 and n["judge"] == 0:
        mark = "↓ 1→0"
    if mark or o["abstained"] != n["abstained"]:
        print(f"  {q}: judge {o['judge']}→{n['judge']}  abstain {o['abstained']}→{n['abstained']}  {mark}")
        print(f"    Q: {o['question'][:80]}")
        print(f"    AN: {str(n['answer'])[:100]}")
