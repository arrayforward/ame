#!/usr/bin/env python3
# 失败分类统计：读 ame_bench --dump 的 NDJSON，按四类归因汇总
# 用法: python3 bench/scripts/analyze_dump.py bench/runs/locomo_llm_v1.ndjson
import json, sys, collections

path = sys.argv[1]
cats = collections.defaultdict(lambda: collections.Counter())
examples = collections.defaultdict(list)

for line in open(path):
    if not line.strip():
        continue
    r = json.loads(line)
    cat = r.get("category") or r.get("qtype")
    gold_ev = set(r.get("gold_evidence") or [])
    got_ev = set(r.get("evidence_dia_ids") or [])
    recalled = bool(gold_ev & got_ev) if gold_ev else None  # None=无证据标注(对抗题)
    abst = r["abstained"]
    judge = r.get("judge")
    unans = r.get("unanswerable", False)

    if unans:
        cls = "adversarial_correct_abstain" if abst else "adversarial_wrong_answer"
    elif recalled is False:
        cls = "retrieval_miss"            # 检索未召回证据
    elif abst:
        cls = "recalled_but_abstained"    # 召回了但弃答
    elif judge == 1.0:
        cls = "correct"
    elif judge == 0.0:
        cls = "answered_wrong"            # 召回答了但答错（含judge误判候选）
    else:
        cls = "no_judge"
    cats[cat][cls] += 1
    if cls in ("retrieval_miss", "recalled_but_abstained", "answered_wrong",
               "adversarial_wrong_answer") and len(examples[cls]) < 5:
        examples[cls].append(r)

CLASSES = ["correct", "retrieval_miss", "recalled_but_abstained", "answered_wrong",
           "adversarial_correct_abstain", "adversarial_wrong_answer", "no_judge"]
print(f"{'cat':>6} {'n':>4}", "  ".join(f"{c[:18]:>18}" for c in CLASSES))
tot = collections.Counter()
for cat in sorted(cats, key=str):
    c = cats[cat]
    n = sum(c.values())
    tot.update(c)
    print(f"{str(cat):>6} {n:>4}", "  ".join(f"{c.get(x,0):>18}" for x in CLASSES))
n = sum(tot.values())
print(f"{'ALL':>6} {n:>4}", "  ".join(f"{tot.get(x,0):>18}" for x in CLASSES))

for cls, exs in examples.items():
    print(f"\n===== {cls} 样例 =====")
    for r in exs:
        print("Q:", r["question"][:110])
        print("  gold:", (r["reference_answer"] or "")[:90], "| ans:", r["answer"][:90],
              "| abst:", r["abstained"], "| judge:", r.get("judge"))
        print("  gold_ev:", r.get("gold_evidence"), "| got:", (r.get("evidence_dia_ids") or [])[:5])
        for ev in (r.get("evidence_top3") or [])[:2]:
            print("   ev:", ev[:110])
