#!/usr/bin/env python3
# LME 三类题型抽样（固定种子 seed=42，可复现）：temporal 2 + knowledge-update 2 + abstention(_abs) 2
# 输出: bench/data/lme_type_sample.json（LongMemEval 格式）+ 选题清单
import json, random

d = json.load(open("/mnt/d/agent/memory/ame/bench/data/longmemeval_s_cleaned.json"))
rng = random.Random(42)

temporal = [x for x in d if x["question_type"] == "temporal-reasoning" and "_abs" not in x["question_id"]]
ku = [x for x in d if x["question_type"] == "knowledge-update" and "_abs" not in x["question_id"]]
abst = [x for x in d if "_abs" in x["question_id"].lower()]

sel = rng.sample(temporal, 2) + rng.sample(ku, 2) + rng.sample(abst, 2)
out = []
for q in sel:
    q2 = dict(q)
    q2["sample_role"] = ("abstention" if "_abs" in q["question_id"].lower()
                         else q["question_type"])
    out.append(q2)
json.dump(out, open("/mnt/d/agent/memory/ame/bench/data/lme_type_sample.json", "w"),
          ensure_ascii=False, indent=1)
for q in out:
    print(q["question_id"], "|", q["sample_role"], "|", q["question"][:70])
