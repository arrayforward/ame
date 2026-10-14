#!/usr/bin/env python3
# P0 验收抽样对照：graph（1.x Engine）vs lmem（2.0 分层存储 M15–M19）
# 双后端跑同一题单/同缓存，逐题对照 evidence_dia_ids 覆盖率（Recall@k），落 bench/runs/lmem_p0/。
# 题单：LoCoMo 既有 sample50（cat1~5 各 10，cat2 时序题必入，make_sample50.py 先例）+
#       三夹具（locomo/longmemeval/beam mini）。LLM 答题/判分走 bench/cache 磁盘缓存；
#       无 key/无网时退化为只比召回覆盖率（judge 标 N/A）——覆盖率是本任务硬指标。
# 用法：python bench/scripts/bench_compare_sample.py [--llm] [--quick]
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent  # ame/（bench/scripts/x.py 的上上级）
EXE = ROOT / "build-win/bench/ame_bench.exe"
OUT = ROOT / "bench/runs/lmem_p0"
SAMPLE_IDS = ROOT / "bench/data/locomo_sample50.json"
CACHE = ROOT / "bench/cache"

# 四配置：三夹具 + LoCoMo sample50（数据缺哪个自动跳过并在汇总注明）
CONFIGS = [
    ("locomo_mini", "locomo", ROOT / "bench/fixtures/locomo_mini.json", None),
    ("longmemeval_mini", "longmemeval", ROOT / "bench/fixtures/longmemeval_mini.json", None),
    ("beam_mini", "beam", ROOT / "bench/fixtures/beam_mini.json", None),
    ("locomo_sample50", "locomo", ROOT / "bench/data/locomo10.json", SAMPLE_IDS),
]


def have_llm_key():
    cfg = ROOT / "bench/config/deepseek.local.json"
    if os.environ.get("AME_DEEPSEEK_API_KEY"):
        return True
    try:
        j = json.loads(cfg.read_text(encoding="utf-8"))
        return bool(j.get("api_key"))
    except Exception:
        return False


def run_one(store, name, dataset, data, qids, use_llm, logdir):
    """单次评测；返回 (elapsed, dump_path) 或 None（数据缺失）。"""
    if not data.exists():
        return None
    dump = logdir / f"{name}.ndjson"
    out = logdir / f"{name}.json"
    log = logdir / f"{name}.log"
    cmd = [str(EXE), "--dataset", dataset, "--data", str(data),
           "--store", store, "--dump", str(dump), "--out", str(out)]
    if qids:
        cmd += ["--question-ids", str(qids)]
    if use_llm:
        cmd += ["--answer", "llm", "--judge", "llm", "--cache-dir", str(CACHE)]
    env = dict(os.environ)
    # DLL 加载纪律：exe 动态链 libstdc++-6.dll，PATH 中若先命中其他 mingw（Git 自带）
    # 会 0xC0000139（STATUS_ENTRYPOINT_NOT_FOUND）。外层 bash 用 AME_MINGW_BIN 传入
    # 构建所用的 mingw bin（其前置 PATH 即 cmake 构建环境）；缺省退回子 shell 探测。
    mingw = os.environ.get("AME_MINGW_BIN", "")
    if not mingw:
        r = subprocess.run(["bash", "-c", "dirname $(which g++)"],
                           capture_output=True, text=True)
        mingw = r.stdout.strip()
    if mingw:
        env["PATH"] = mingw + os.pathsep + env.get("PATH", "")
    t0 = time.time()
    with open(log, "w", encoding="utf-8") as lf:
        rc = subprocess.run(cmd, cwd=ROOT, stdout=lf, stderr=lf, env=env).returncode
    return time.time() - t0, dump, out, rc


def load_dump(path):
    rows = {}
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            r = json.loads(line)
            rows[r["question_id"]] = r
    return rows


def coverage(row):
    """evidence_dia_ids 覆盖率 = |gold ∩ evidence| / |gold|；金标为空返回 None。"""
    gold = row.get("gold_evidence") or []
    ev = set(row.get("evidence_dia_ids") or [])
    if not gold:
        return None
    hit = sum(1 for g in gold if g in ev)
    return hit / len(gold), hit, len(gold)


def fmt(x, nd=3):
    return "-" if x is None else f"{x:.{nd}f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--llm", action="store_true", help="LLM 答题+判分（走磁盘缓存；失败自动退化为 baseline/f1）")
    ap.add_argument("--quick", action="store_true", help="只跑 LoCoMo sample50")
    args = ap.parse_args()

    if not EXE.exists():
        sys.exit(f"缺少 {EXE}，先 cmake --build build-win")
    use_llm = args.llm and have_llm_key()
    if args.llm and not use_llm:
        print("[warn] 无 API key（config/env），退化为只比召回覆盖率（judge=N/A）")

    configs = [c for c in CONFIGS if not args.quick or c[0] == "locomo_sample50"]
    results = {}   # (store, name) -> {elapsed, rows, rc}
    total_t0 = time.time()
    for name, dataset, data, qids in configs:
        for store in ("graph", "lmem"):
            logdir = OUT / (store + ("_llm" if use_llm else ""))
            logdir.mkdir(parents=True, exist_ok=True)
            r = run_one(store, name, dataset, data, qids, use_llm, logdir)
            if r is None:
                print(f"[skip] {name}: 数据缺失 {data}")
                continue
            elapsed, dump, out, rc = r
            results[(store, name)] = {"elapsed": elapsed, "rows": load_dump(dump), "rc": rc}
            print(f"[run] {store:5s} {name:18s} rc={rc} {elapsed:.1f}s "
                  f"({len(results[(store, name)]['rows'])} 题)")

    # ---- 逐题对照表 + 汇总 ----
    lines = []
    lines.append(f"# 双后端 evidence_dia_ids 覆盖率对照（llm={use_llm}）\n")
    header = f"{'question_id':<18} {'cat':>3} {'cov_g':>6} {'cov_l':>6} {'hit_g':>5} {'hit_l':>5} {'judge_g':>7} {'judge_l':>7}"
    for name, dataset, data, qids in configs:
        g = results.get(("graph", name), {}).get("rows", {})
        l = results.get(("lmem", name), {}).get("rows", {})
        if not g and not l:
            continue
        lines.append(f"\n## {name}\n")
        lines.append("```")
        lines.append(header)
        cov_g_all, cov_l_all = [], []
        bycat = {}
        for qid in sorted(set(g) | set(l)):
            gr, lr = g.get(qid), l.get(qid)
            cg = coverage(gr) if gr else None
            cl = coverage(lr) if lr else None
            if cg: cov_g_all.append(cg[0])
            if cl: cov_l_all.append(cl[0])
            cat = (gr or lr).get("category", 0)
            if cg or cl:
                bycat.setdefault(cat, []).append((cg[0] if cg else None, cl[0] if cl else None))
            jg = (gr or {}).get("judge")
            jl = (lr or {}).get("judge")
            lines.append(f"{qid:<18} {cat:>3} "
                         f"{fmt(cg[0] if cg else None):>6} {fmt(cl[0] if cl else None):>6} "
                         f"{(str(cg[1])+'/'+str(cg[2])) if cg else '-':>5} "
                         f"{(str(cl[1])+'/'+str(cl[2])) if cl else '-':>5} "
                         f"{fmt(jg, 1):>7} {fmt(jl, 1):>7}")
        mg = sum(cov_g_all) / len(cov_g_all) if cov_g_all else None
        ml = sum(cov_l_all) / len(cov_l_all) if cov_l_all else None
        lines.append("```")
        lines.append(f"- 覆盖率均值: graph={fmt(mg)} lmem={fmt(ml)} "
                     f"(n={len(cov_g_all)}; lmem-graph={fmt((ml - mg) if (mg is not None and ml is not None) else None, 3)})")
        for cat in sorted(bycat):
            bg = [a for a, b in bycat[cat] if a is not None]
            bl = [b for a, b in bycat[cat] if b is not None]
            lines.append(f"- category_{cat}: graph={fmt(sum(bg)/len(bg) if bg else None)} "
                         f"lmem={fmt(sum(bl)/len(bl) if bl else None)} (n={len(bg)})")
        tg = results.get(("graph", name), {}).get("elapsed", 0)
        tl = results.get(("lmem", name), {}).get("elapsed", 0)
        lines.append(f"- 耗时: graph={tg:.1f}s lmem={tl:.1f}s")

    total = time.time() - total_t0
    lines.append(f"\n端到端总耗时: {total/60:.1f} min（时间盒 ≤60min）")
    report = "\n".join(lines)
    table_name = "compare_table_llm.md" if use_llm else "compare_table.md"
    (OUT / table_name).write_text(report, encoding="utf-8")
    print("\n" + report)
    print(f"\n[done] 对照表落盘 {OUT/table_name}")


if __name__ == "__main__":
    main()
