#!/usr/bin/env python3
# 写时增强对照实验（预注册）：同冻结配置（--entity-pass-threshold 0.5
# --rerank-guard 0.2）三档终排 × {原始, 增强} 语料，sample50 覆盖率对照。
# 判据（任务书预注册）：bm25/cheap 增强版覆盖率提升 > +0.03 且零推理成本
#   → 判"写时增强成立"；≤ +0.01 → 判不成立，归档并如实报。
# 覆盖率口径与 bench_compare_sample.py / llm_rerank_spike 一致：
#   每题 |gold ∩ evidence_dia_ids| / |gold|，空金标剔除后取均值；附 any-hit。
# 用法：python bench/scripts/enrich_ab.py [--llm]
import argparse
import json
import os
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
EXE = ROOT / "build-win/bench/ame_bench.exe"
OUT = ROOT / "bench/runs/enrich"
QIDS = ROOT / "bench/data/locomo_sample50.json"
RAW = ROOT / "bench/data/locomo10.json"
ENRICHED = ROOT / "bench/data/locomo10_enriched.json"
CHEAP_MODEL = ROOT / "bench/runs/mechanisms/cheap_model_v2.json"
CACHE = ROOT / "bench/cache"
CONFIG = ROOT / "bench/config/deepseek.local.json"
FROZEN = ["--entity-pass-threshold", "0.5", "--rerank-guard", "0.2"]


def run_one(name, data, extra):
    dump = OUT / f"{name}.ndjson"
    cmd = [str(EXE), "--dataset", "locomo", "--data", str(data),
           "--question-ids", str(QIDS), "--extractor", "rule",
           "--answer", "baseline", "--judge", "f1", "--k", "10",
           "--config", str(CONFIG), "--cache-dir", str(CACHE),
           "--dump", str(dump)] + FROZEN + extra
    env = dict(os.environ)
    mingw = env.get("AME_MINGW_BIN", "")
    if not mingw:
        r = subprocess.run(["bash", "-c", "dirname $(which g++)"],
                           capture_output=True, text=True)
        mingw = r.stdout.strip()
    if mingw:
        env["PATH"] = mingw + os.pathsep + env.get("PATH", "")
    t0 = time.time()
    with open(OUT / f"{name}.log", "w", encoding="utf-8") as lf:
        rc = subprocess.run(cmd, cwd=ROOT, stdout=lf, stderr=lf, env=env).returncode
    rows = {}
    if dump.exists():
        for line in dump.read_text(encoding="utf-8").splitlines():
            if line.strip():
                r = json.loads(line)
                rows[r["question_id"]] = r
    return rc, time.time() - t0, rows


def cov(row):
    gold = row.get("gold_evidence") or []
    ev = set(row.get("evidence_dia_ids") or [])
    if not gold:
        return None
    hit = sum(1 for g in gold if g in ev)
    return hit / len(gold), hit, len(gold)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--llm", action="store_true", help="追加 llm 档×2（需 API）")
    ap.add_argument("--only", default="", help="只跑某档：bm25|cheap|llm")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)

    runs = [("bm25", []), ("cheap", ["--rerank", "cheap", "--cheap-rerank", str(CHEAP_MODEL)])]
    if args.llm:
        runs.append(("llm", ["--rerank", "llm"]))
    if args.only:
        runs = [r for r in runs if r[0] == args.only]

    res = {}
    for tier, extra in runs:
        for variant, data in (("raw", RAW), ("enr", ENRICHED)):
            name = f"{tier}_{variant}"
            if not data.exists():
                print(f"[skip] {name}: 数据缺失 {data}")
                continue
            rc, el, rows = run_one(name, data, extra)
            res[name] = rows
            cs = [cov(r)[0] for r in rows.values() if cov(r)]
            ah = sum(1 for r in rows.values() if cov(r) and cov(r)[1] > 0)
            print(f"[run] {name}: rc={rc} {el:.1f}s cov={sum(cs)/len(cs):.3f} "
                  f"any-hit={ah}/{len(cs)}")

    # ---- 对照表 ----
    qids = sorted(set().union(*[set(r) for r in res.values()])) if res else []
    lines = ["# 写时增强 A/B 对照（冻结配置 entity-pass 0.5 + guard 0.2，sample50）\n",
             f"{'question_id':<18} {'cat':>3}" +
             "".join(f" {n:>10}" for n in res)]
    agg = {n: [] for n in res}
    ah = {n: 0 for n in res}
    for qid in qids:
        row = f"{qid:<18} {(res[next(iter(res))].get(qid) or {}).get('category', 0):>3}"
        for n, rows in res.items():
            c = cov(rows.get(qid)) if rows.get(qid) else None
            if c:
                agg[n].append(c[0])
                ah[n] += 1 if c[1] > 0 else 0
            row += f" {c[0] if c else float('nan'):>10.3f}"
        lines.append(row)
    lines.append("")
    for tier, _ in runs:
        a, b = f"{tier}_raw", f"{tier}_enr"
        if a in agg and b in agg and agg[a] and agg[b]:
            ma, mb = sum(agg[a]) / len(agg[a]), sum(agg[b]) / len(agg[b])
            lines.append(f"- **{tier}**: raw={ma:.3f} (any-hit {ah[a]}/{len(agg[a])}) "
                         f"enr={mb:.3f} (any-hit {ah[b]}/{len(agg[b])}) Δ={mb - ma:+.3f}")
    table = "\n".join(lines)
    (OUT / "ab_table.md").write_text(table, encoding="utf-8")
    print("\n" + table)
    print(f"\n[done] 落盘 {OUT/'ab_table.md'}")


if __name__ == "__main__":
    main()
