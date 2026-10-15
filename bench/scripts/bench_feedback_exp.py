#!/usr/bin/env python3
# P2b 双 pass 反馈实验：--store lmem --fb-exp（judge>=1 → 正反馈，否则负反馈，同 1.x 语义）
#   pass1：正常 LLM 答题 + LLM 判分 → 按题回流 feedback 到证据来源 kind（落盘 reliability.json）
#   pass2：同目录重开 Mem（replay 恢复观测/事实/可靠性），同题单只跑召回（零 LLM 调用），
#          对比金标证据在 pass1/pass2 证据序中的首位命中 rank（上升/不变/下降）
# 报告：逐题 [fbexp] 行 + [fbexp-summary] 汇总 → bench/runs/lmem_p2/feedback_rank_report.md
# 用法：python bench/scripts/bench_feedback_exp.py [--quick]（默认 --llm 判分，走 bench/cache）
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
EXE = ROOT / "build-win/bench/ame_bench.exe"
OUT = ROOT / "bench/runs/lmem_p2"
FB_DIR = OUT / "fbexp_data"      # 每 case 的 lmem 分片根（脚本每次先清空，防重复摄入）
SAMPLE_IDS = ROOT / "bench/data/locomo_sample50.json"
CACHE = ROOT / "bench/cache"

CONFIGS = [
    ("locomo_mini", "locomo", ROOT / "bench/fixtures/locomo_mini.json", None),
    ("longmemeval_mini", "longmemeval", ROOT / "bench/fixtures/longmemeval_mini.json", None),
    ("beam_mini", "beam", ROOT / "bench/fixtures/beam_mini.json", None),
    ("locomo_sample50", "locomo", ROOT / "bench/data/locomo10.json", SAMPLE_IDS),
]

LINE_RE = re.compile(r"^\[fbexp\] (\S+) (\S+) gold_rank pass1=(\d+) pass2=(\d+) (\w+)$")
SUM_RE = re.compile(r"^\[fbexp-summary\] questions=(\d+) up=(\d+) same=(\d+) down=(\d+) nogold=(\d+)$")


def mingw_bin():
    v = os.environ.get("AME_MINGW_BIN", "")
    if v:
        return v
    r = subprocess.run(["bash", "-c", "dirname $(which g++)"], capture_output=True, text=True)
    return r.stdout.strip()


def run_one(name, dataset, data, qids, use_llm, logdir):
    if not data.exists():
        return None
    dump = logdir / f"{name}.ndjson"
    out = logdir / f"{name}.json"
    log = logdir / f"{name}.log"
    cmd = [str(EXE), "--dataset", dataset, "--data", str(data),
           "--store", "lmem", "--fb-exp", str(FB_DIR),
           "--dump", str(dump), "--out", str(out)]
    if qids:
        cmd += ["--question-ids", str(qids)]
    if use_llm:
        cmd += ["--answer", "llm", "--judge", "llm", "--cache-dir", str(CACHE)]
    env = dict(os.environ)
    m = mingw_bin()
    if m:
        env["PATH"] = m + os.pathsep + env.get("PATH", "")
    t0 = time.time()
    with open(log, "w", encoding="utf-8") as lf:
        rc = subprocess.run(cmd, cwd=ROOT, stdout=lf, stderr=lf, env=env).returncode
    return time.time() - t0, dump, log, rc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true", help="只跑 LoCoMo sample50")
    ap.add_argument("--no-llm", action="store_true", help="不判分（反馈为零，仅供冒烟）")
    args = ap.parse_args()
    if not EXE.exists():
        sys.exit(f"缺少 {EXE}，先 cmake --build build-win")
    use_llm = not args.no_llm

    OUT.mkdir(parents=True, exist_ok=True)
    shutil.rmtree(FB_DIR, ignore_errors=True)  # 幂等：清旧分片，防重复摄入

    configs = [c for c in CONFIGS if not args.quick or c[0] == "locomo_sample50"]
    lines = ["# P2b 双 pass 反馈实验：金标证据 rank 变化（pass1 判分→feedback→pass2 只召回）\n"]
    tot = {"up": 0, "same": 0, "down": 0, "nogold": 0}
    grand_q = 0
    t_start = time.time()
    for name, dataset, data, qids in configs:
        r = run_one(name, dataset, data, qids, use_llm, OUT)
        if r is None:
            print(f"[skip] {name}: 数据缺失 {data}")
            continue
        elapsed, dump, log, rc = r
        rows, summary = [], None
        for ln in Path(log).read_text(encoding="utf-8", errors="replace").splitlines():
            m = LINE_RE.match(ln.strip())
            if m:
                rows.append(m.groups())
                continue
            s = SUM_RE.match(ln.strip())
            if s:
                summary = s.groups()
        # 与判分结果（dump ndjson）对齐，标注 judge
        judge = {}
        if dump.exists():
            for ln in dump.read_text(encoding="utf-8").splitlines():
                if ln.strip():
                    d = json.loads(ln)
                    judge[d["question_id"]] = d.get("judge")
        lines.append(f"\n## {name}（rc={rc}, {elapsed:.1f}s）\n")
        lines.append("```")
        lines.append(f"{'question_id':<18} {'judge':>5} {'rank_p1':>7} {'rank_p2':>7}  dir")
        for conv, qid, r1, r2, d in rows:
            j = judge.get(qid)
            lines.append(f"{qid:<18} {('N/A' if j is None else str(j)):>5} "
                         f"{r1:>7} {r2:>7}  {d}")
        lines.append("```")
        if summary:
            q, up, same, down, nogold = summary
            lines.append(f"- 汇总: questions={q} up={up} same={same} down={down} nogold={nogold}")
            for k in tot:
                tot[k] += int(summary[{"up": 1, "same": 2, "down": 3, "nogold": 4}[k]])
            grand_q += int(q)
            print(f"[run] {name}: rc={rc} {elapsed:.1f}s up={up} same={same} down={down} nogold={nogold}")
        else:
            lines.append("- 汇总: 未捕获 [fbexp-summary]（见 log）")
            print(f"[run] {name}: rc={rc} {elapsed:.1f}s (无 fbexp 输出，见 {log})")
    lines.append(f"\n## 总计\n\n- questions={grand_q} up={tot['up']} same={tot['same']} "
                 f"down={tot['down']} nogold={tot['nogold']}")
    lines.append(f"- 端到端耗时: {(time.time() - t_start) / 60:.1f} min（时间盒 ≤60min）")
    report = "\n".join(lines)
    (OUT / "feedback_rank_report.md").write_text(report, encoding="utf-8")
    print("\n" + report)
    print(f"\n[done] 报告落盘 {OUT/'feedback_rank_report.md'}")


if __name__ == "__main__":
    main()
