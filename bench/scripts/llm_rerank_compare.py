#!/usr/bin/env python3
# LLM 重排 spike 判决实验 A/B 对照：off dump vs --llm-rerank dump 的逐题覆盖率对照。
# 口径（同 lexicon_act_compare.py / lmem_p0）：每题 coverage = |gold ∩ evidence_dia_ids| / |gold|；
# 金标为空跳过；总口径 = 有金标题的覆盖率均值。
# 附带：exp 运行日志的 [llm-rerank] 逐题状态（LLM 置顶 n/N 或降级 BM25）。
# 用法: python bench/scripts/llm_rerank_compare.py off.ndjson exp.ndjson exp.log
import json
import re
import sys

# lmem_p3 miss taxonomy 的分类标签（D1 口径，作为本题级 taxonomy 参照；
# 该表为 lmem 候选池口径，graph 管道池序不同，仅作分类标签引用）
TAXONOMY = {
    "conv-26:q3": "A", "conv-26:q18": "A", "conv-26:q64": "A",
    "conv-26:q105": "A", "conv-26:q156": "A",
    "conv-26:q10": "B", "conv-26:q101": "B", "conv-26:q154": "B",
    "conv-26:q155": "B", "conv-26:q34": "B", "conv-26:q50": "B",
}
NEAR_MISS = {"conv-26:q104": "q104型近失(lmem pool_rank 8)"}


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


def rerank_status(log_path):
    """[llm-rerank] <qid>: LLM 置顶 n/N 条  /  LLM 无有效输出，降级 BM25 原序"""
    st = {}
    if not log_path:
        return st
    rx = re.compile(r"\[llm-rerank\]\s+(\S+?):\s+(.*)")
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = rx.search(line)
            if m:
                st[m.group(1)] = m.group(2).strip()
    return st


def main():
    off_path, exp_path = sys.argv[1], sys.argv[2]
    log_path = sys.argv[3] if len(sys.argv) > 3 else None
    A, B = load(off_path), load(exp_path)
    st = rerank_status(log_path)
    ids = sorted(set(A) | set(B))
    ca = cb = 0.0
    na = nb = ah_a = ah_b = 0
    print(f"{'qid':<16}{'cat':<4}{'tax':<5}{'cov_off':<9}{'cov_exp':<9}{'Δ':<8}rerank 状态")
    rescued, lost = [], []
    for qid in ids:
        ra, rb = A.get(qid), B.get(qid)
        ca_v = coverage(ra) if ra else None
        cb_v = coverage(rb) if rb else None
        tax = TAXONOMY.get(qid, NEAR_MISS.get(qid, ""))
        mark = ""
        if ca_v is not None and cb_v is not None:
            d = cb_v - ca_v
            if d > 0:
                mark = "← 救回"
                rescued.append(qid)
            elif d < 0:
                mark = "← 丢失"
                lost.append(qid)
        if ca_v is not None:
            ca += ca_v
            na += 1
            ah_a += any_hit(ra)
        if cb_v is not None:
            cb += cb_v
            nb += 1
            ah_b += any_hit(rb)
        cat = (ra or rb).get("category", "?")
        rstat = st.get(qid, "-（通道未触发/实体通道提前返回）")
        row = f"{qid:<16}{cat:<4}{tax:<5}"
        row += f"{ca_v if ca_v is not None else '-':<9}"
        row += f"{cb_v if cb_v is not None else '-':<9}"
        row += f"{(cb_v - ca_v) if (ca_v is not None and cb_v is not None) else '-':<8}"
        row += f"{rstat} {mark}"
        print(row)
    print()
    print(f"覆盖率均值: off={ca/na:.3f} (n={na}, any-hit {ah_a})  "
          f"exp={cb/nb:.3f} (n={nb}, any-hit {ah_b})  Δ={cb/nb - ca/na:+.3f}")
    print(f"救回: {rescued}")
    print(f"丢失: {lost}")
    engaged = [k for k, v in st.items() if "置顶" in v]
    degraded = [k for k, v in st.items() if "降级" in v]
    print(f"重排触发 {len(st)} 题（置顶 {len(engaged)}，降级 {len(degraded)}）")
    if degraded:
        print(f"降级题: {sorted(degraded)}")


if __name__ == "__main__":
    main()
