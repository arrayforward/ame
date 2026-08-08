#!/usr/bin/env python3
# 固定 50 题样本集：cat1~5 各取 question_id 排序前 10（不参考答案内容，避免选题偏差）
import json, collections

seen = collections.defaultdict(list)
for line in open("bench/runs/locomo_llm_v1.ndjson"):
    r = json.loads(line)
    seen[r["category"]].append(r["question_id"])
sel = []
for cat in sorted(seen):
    sel.extend(sorted(seen[cat])[:10])
json.dump(sel, open("bench/data/locomo_sample50.json", "w"), indent=0)
print("sample size:", len(sel), collections.Counter(s.split(":q")[0] for s in sel).most_common(3))
