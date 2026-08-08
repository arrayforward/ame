#!/bin/bash
# 检查 LongMemEval haystack 共享度（决定能否按 haystack 分组复用 Engine）
cd /mnt/d/agent/memory/ame
python3 - <<'PYEOF'
import json
d = json.load(open("bench/data/longmemeval_s_cleaned.json"))
sigs = {}
for item in d:
    sig = tuple(item.get("haystack_session_ids", []))
    sigs.setdefault(sig, []).append(item["question_id"])
print("cases:", len(d), "distinct haystacks:", len(sigs))
sizes = sorted((len(v) for v in sigs.values()), reverse=True)
print("questions per haystack top10:", sizes[:10])
turns = [sum(len(s) for s in item["haystack_sessions"]) for item in d]
print("turns per case min/avg/max:", min(turns), sum(turns)//len(turns), max(turns))
PYEOF
