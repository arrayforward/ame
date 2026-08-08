#!/usr/bin/env python3
# 中文同义词词林 + 反义词库 → AME lexicon_zh.json（离线，零 LLM）
# 输入: bench/data/synonyms_zh.txt（编码+标记符=词组："="同义组、"#"近义/相关组、"@"独词）
#       bench/data/antonyms_zh.txt（"前——后" 反义词对，互指）
# 输出: bench/data/lexicon_zh.json（三分流词条格式 syn_strong/syn_near/antonyms）
# 说明：只按词面处理同字多音/多义词，不做词义消歧（通用机制；误归一由条件归一/
#       扩散传导系数兜底）。
import json, sys, collections

BASE = "/mnt/d/agent/memory/ame/bench/data"
# 可复用：convert_zh.py [同义组文件 反义词文件|- 输出文件]
SYN_IN = sys.argv[1] if len(sys.argv) > 1 else f"{BASE}/synonyms_zh.txt"
ANT_IN = sys.argv[2] if len(sys.argv) > 2 else f"{BASE}/antonyms_zh.txt"
OUT = sys.argv[3] if len(sys.argv) > 3 else f"{BASE}/lexicon_zh.json"

strong_of = collections.defaultdict(list)   # word -> [syn_strong]
near_of = collections.defaultdict(list)     # word -> [syn_near]
ant_of = collections.defaultdict(list)      # word -> [antonyms]

n_strong_groups = n_near_groups = n_skip = 0
for line in open(SYN_IN, encoding="utf-8"):
    line = line.strip()
    if not line:
        continue
    if line.startswith("G= "):
        # organize_dicts 伪编码组（箭头对/自由组）：标准词=组内首词
        marker, body = "=", line[3:]
    else:
        marker = line[7] if len(line) > 7 else ""
        body = line[8:].strip() if len(line) > 8 else ""
    words = [w for w in body.split() if w]
    if marker == "=" and len(words) >= 2:
        # 同义组：组内首词为全组标准词（canonical），成员统一归一并到首词——
        # 避免组内两两互指产生归一链/环
        n_strong_groups += 1
        std = words[0]
        strong_of[std].extend(x for x in words[1:])
        for w in words[1:]:
            strong_of[w].append(std)
    elif marker == "#" and len(words) >= 2:
        # 近义组：同样以组内首词为锚点（近义边节点保留，锚点只为条目组织服务）
        n_near_groups += 1
        std = words[0]
        near_of[std].extend(x for x in words[1:])
        for w in words[1:]:
            near_of[w].append(std)
    else:                                       # "@" 独词/无标记 → 跳过
        n_skip += 1

n_ant = 0
if ANT_IN != "-":
    for line in open(ANT_IN, encoding="utf-8"):
        line = line.strip()
        a = b = ""
        for sep in ("——", "--", "—", "-"):  # 分隔符四种变体，长者优先
            if sep in line:
                a, b = line.split(sep, 1)
                break
        a, b = a.strip(), b.strip()
        if not a or not b:
            continue
        n_ant += 1
        ant_of[a].append(b)
        ant_of[b].append(a)

words = set(strong_of) | set(near_of) | set(ant_of)
entries = []
for w in sorted(words):
    entries.append({
        "word": w,
        "syn_strong": list(dict.fromkeys(strong_of.get(w, [])))[:8],
        "syn_near": [{"word": x, "score": 0.7} for x in dict.fromkeys(near_of.get(w, []))][:8],
        "antonyms": list(dict.fromkeys(ant_of.get(w, [])))[:4],
    })
json.dump({"version": "cilin-funNLP", "entries": entries},
          open(OUT, "w", encoding="utf-8"), ensure_ascii=False)

strong_pairs = sum(len(e["syn_strong"]) for e in entries)
near_pairs = sum(len(e["syn_near"]) for e in entries)
ant_pairs = sum(len(e["antonyms"]) for e in entries)
print(f"同义组={n_strong_groups} 近义组={n_near_groups} 跳过={n_skip} 反义对={n_ant}")
print(f"entries={len(entries)} strong={strong_pairs} near={near_pairs} ant={ant_pairs}")
# 抽样验证：睡不着组
for e in entries:
    if e["word"] == "失眠":
        print("失眠:", e)
        break
