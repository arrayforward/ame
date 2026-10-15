#!/usr/bin/env python3
# oracle_ceiling 三实验汇总分析：E1/E2 逐题表 + E3 实体通道净贡献 + 天花板计算。
# 输入: bench/runs/oracle_ceiling/{e1,e2,e3_on,e3_off}.ndjson + e3_attr.log
# 口径: coverage = |gold ∩ evidence_dia_ids| / |gold|，有金标题均值（同 lexicon_act_compare）
import json
import re
import sys

RUNS = "bench/runs/oracle_ceiling"
TAX = {"conv-26:q3": "A", "conv-26:q18": "A", "conv-26:q64": "A", "conv-26:q105": "A",
       "conv-26:q156": "A", "conv-26:q10": "B", "conv-26:q101": "B", "conv-26:q154": "B",
       "conv-26:q155": "B", "conv-26:q34": "B", "conv-26:q50": "B", "conv-26:q104": "q104"}


def load(p):
    return {json.loads(l)["question_id"]: json.loads(l) for l in open(p, encoding="utf-8") if l.strip()}


def cov(r):
    gold, ev = r.get("gold_evidence") or [], r.get("evidence_dia_ids") or []
    return (len(set(gold) & set(ev)) / len(gold)) if gold else None


def main():
    e1 = load(f"{RUNS}/e1.ndjson")
    e2 = load(f"{RUNS}/e2.ndjson")
    on, off = load(f"{RUNS}/e3_on.ndjson"), load(f"{RUNS}/e3_off.ndjson")
    # 实体通道截走归因（--llm-rerank 20 运行日志）
    intercepted = set()
    for line in open(f"{RUNS}/e3_attr.log", encoding="utf-8", errors="replace"):
        m = re.search(r"\[llm-rerank\]\s+(\S+?):\s+实体档案通道提前返回", line)
        if m:
            intercepted.add(m.group(1))

    print("== E1 B 类整池 LLM top-10 重选（池级语义排序天花板）==")
    b_in, b_pick = 0, 0
    for qid in sorted(e1):
        r = e1[qid]
        pool = {p["dia_id"]: p["bm25_rank"] for p in r["pool"]}
        golds = r["gold"]
        gin = [(g, pool[g]) for g in golds if g in pool]
        picked = r["gold_picked"]
        b_in += bool(gin)
        b_pick += bool(picked)
        print(f"  {qid} ({TAX.get(qid,'?')}): 金标在池={gin if gin else '无'} "
              f"→ LLM top-10 入选={picked if picked else '否'}")
    print(f"  → 金标在池 {b_in}/6，入选 {b_pick}/6（在池者入选率 {b_pick}/{b_in}）")

    print("\n== E2 A 类 LLM 查询扩展进池（扩池天花板）==")
    a_new, a_reach = 0, 0
    for qid in sorted(e2):
        r = e2[qid]
        new = r["gold_new_in_union"]
        orig = int(r["orig_gold_in_pool"]) > 0
        a_new += bool(new)
        a_reach += bool(new) or orig
        print(f"  {qid} ({TAX.get(qid,'?')}): 原池金标={r['orig_gold_in_pool']} "
              f"扩展新进={new if new else '无'} 命中查询={r['gold_hit_queries']}/3 "
              f"新噪声={r['new_non_gold_count']} 并池={r['union_pool_size']}")
    print(f"  → 扩展新进池 {a_new}/5，池可达(含原池) {a_reach}/5")

    print("\n== E3 实体档案通道净贡献审计 ==")
    print(f"  截走题数: {len(intercepted)}/50")
    rows = []
    for qid in sorted(intercepted):
        ro, rf = on.get(qid), off.get(qid)
        if not ro or not rf:
            continue
        co, cf = cov(ro), cov(rf)
        jo = ro.get("judge")
        jf = rf.get("judge")
        rows.append((qid, co, cf, jo, jf, ro.get("abstained"), rf.get("abstained")))
    chan_win = [r for r in rows if (r[1] or 0) > (r[2] or 0)]   # on > off：通道受益
    chan_lose = [r for r in rows if (r[2] or 0) > (r[1] or 0)]  # off > on：通道受损
    same = [r for r in rows if (r[2] or 0) == (r[1] or 0)]
    j_on = sum(1 for r in rows if r[3] == 1.0)
    jf_on = sum(1 for r in rows if r[4] == 1.0)
    print(f"  截走题上 coverage: 通道受益 {len(chan_win)} 题 / 通道受损 {len(chan_lose)} 题 / 持平 {len(same)} 题")
    for r in chan_win:
        print(f"    通道受益 {r[0]}: on cov={r[1]} vs off cov={r[2]} | judge on={r[3]} off={r[4]}")
    for r in chan_lose:
        print(f"    通道受损 {r[0]}: on cov={r[1]} vs off cov={r[2]} | judge on={r[3]} off={r[4]}")
    print(f"  截走题 judge 答对: on(实体通道实际) {j_on}/{len(rows)} vs off(标准管道假设) {jf_on}/{len(rows)}")
    # 全样本对照（50 题）
    all_on = [cov(on[q]) for q in on if cov(on[q]) is not None]
    all_off = [cov(off[q]) for q in off if cov(off[q]) is not None]
    print(f"  全样本 coverage: on={sum(all_on)/len(all_on):.3f} (n={len(all_on)}) "
          f"off={sum(all_off)/len(all_off):.3f} (n={len(all_off)}) "
          f"Δ={sum(all_off)/len(all_off)-sum(all_on)/len(all_on):+.3f}")

    print("\n== 天花板总结（锚定当前生产路径 coverage，E3-on 运行实测）==")
    c0 = sum(all_on) / len(all_on)
    b_ceiling = b_pick          # B 类可回收 = LLM 能选中的（假设 rerank 机制达到 oracle 选中率）
    a_ceiling = a_reach         # A 类可回收 = 池可达（含 q105 原池在池）
    n = len(all_on)
    print(f"  当前覆盖率 C0 = {c0:.3f}")
    print(f"  B 类可回收上限 = {b_ceiling}/6 → +{b_ceiling}/{n} = +{b_ceiling/n:.3f}")
    print(f"  A 类可回收上限 = {a_ceiling}/5 → +{a_ceiling}/{n} = +{a_ceiling/n:.3f}")
    print(f"  全部回收 → {c0 + (b_ceiling + a_ceiling)/n:.3f}（vs 词级 plateau 0.464: "
          f"+{c0 + (b_ceiling + a_ceiling)/n - 0.464:+.3f}）")


if __name__ == "__main__":
    main()
