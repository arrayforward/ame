#!/usr/bin/env python3
# P3a miss 分类诊断（判决标准预固定 D1）：
#   对 LoCoMo sample50 全部有金标题，逐题判定 lmem 召回失败模式并分三类——
#     A 改述鸿沟：候选池（Mem.recall 输出，k_pool=60，终排 BM25 之前）无金标
#                 且 问题与金标轮最大词面重合度 < 0.05；
#     B 排序出局：候选池有金标但终排 top-k（k=10）证据无金标；
#     C 其他：    含"重合度 ≥ 0.05 仍无金标"= 词面召回通道漏召回。
#   口径与正式管线一致：同一 exe、同一 --store lmem 取证路径、同一 k_pool；
#   分词与 memindex_tokenize（CJK 二元组 + ASCII 小写词）逐字节等价复刻。
# 产物：docs/reports/lmem_p3_miss_taxonomy.md + bench/runs/lmem_p3/（dump/candidates/log）。
# 口径说明：P3a 的规范口径 = P2 管线（P3b 通道开关关闭，test_lstore ⑦ 验证开关关闭时
# 与 P2 逐比特等价），由 --prox-window 0 --no-expand 复现（D6：最终 commit 一键可复跑）。
# 用法：
#   python bench/scripts/bench_diagnose_misses.py              # P3a 规范口径（默认）
#   python bench/scripts/bench_diagnose_misses.py --post-p3    # P3b 落地后的观测口径
#                                                              #（只落 runs/ 明细，不进 docs 报告）
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
EXE = ROOT / "build-win/bench/ame_bench.exe"
DATA = ROOT / "bench/data/locomo10.json"
SAMPLE_IDS = ROOT / "bench/data/locomo_sample50.json"
OUT = ROOT / "bench/runs/lmem_p3"
REPORT = ROOT / "docs/reports/lmem_p3_miss_taxonomy.md"

# ---- D1 预固定判决标准（先于 P3b/c 实现锁定）----
A_OVERLAP_THRESHOLD = 0.05   # 问题 vs 金标轮 词面重合度阈值（低于 = 改述鸿沟）


# memindex_tokenize 的 Python 逐字节复刻（与 memindex.cpp 同一规则）：
#   ASCII [a-z0-9_]+ 小写词；CJK（UTF-8 三字节头 E4-E9）连续段滑窗二元组，单字退一元。
def memindex_tokenize(text):
    out, word, cjk = [], [], []

    def flush_word():
        if word:
            out.append("".join(word))
            word.clear()

    def flush_cjk():
        if not cjk:
            return
        for i in range(len(cjk) - 1):
            out.append(cjk[i] + cjk[i + 1])
        if len(cjk) == 1:
            out.append(cjk[0])
        cjk.clear()

    b = text.encode("utf-8")
    i = 0
    while i < len(b):
        c = b[i]
        if c < 0x80:
            flush_cjk()
            lc = c - 65 + 97 if 65 <= c <= 90 else c
            if (97 <= lc <= 122) or (48 <= lc <= 57) or lc == 95:
                word.append(chr(lc))
                i += 1
                continue
            flush_word()
            i += 1
            continue
        flush_word()
        if i + 2 < len(b) and 0xE4 <= c <= 0xE9:
            cjk.append(b[i:i + 3].decode("utf-8"))
            i += 3
        else:
            flush_cjk()
            i += 1
    flush_word()
    flush_cjk()
    return out


def overlap(q_text, gold_text):
    """词面重合度 = |tok(q) ∩ tok(gold)| / |tok(q)|（集合口径；空词表返回 0）。"""
    qs = set(memindex_tokenize(q_text))
    if not qs:
        return 0.0
    gs = set(memindex_tokenize(gold_text))
    return len(qs & gs) / len(qs)


def load_gold_texts():
    """sample_id -> {dia_id: turn_text}（conversation.session_N 逐轮）。"""
    samples = json.loads(DATA.read_text(encoding="utf-8"))
    m = {}
    for s in samples:
        conv = s.get("conversation") or {}
        d = {}
        for key, sess in conv.items():
            if not key.startswith("session_") or key.endswith("_date_time"):
                continue
            for turn in sess:
                dia = turn.get("dia_id")
                if dia:
                    d[dia] = turn.get("text", "")
        m[s.get("sample_id")] = d
    return m


def load_ndjson(path):
    rows = {}
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            r = json.loads(line)
            rows[r["question_id"]] = r
    return rows


def run_bench(post_p3):
    OUT.mkdir(parents=True, exist_ok=True)
    tag = "postp3" if post_p3 else "p2mode"
    dump = OUT / f"diag_dump_{tag}.ndjson"
    cands = OUT / f"candidates_{tag}.ndjson"
    log = OUT / f"diag_{tag}.log"
    env = dict(os.environ)
    # DLL 加载纪律（同 bench_compare_sample）：exe 动态链 libstdc++-6.dll，PATH 须以
    # 构建所用 mingw 优先；按 D6 纪律由外层 bash export AME_MINGW_BIN / PATH 传入，
    # 本脚本不自行探测（子 shell 的 which 在本机原生 Windows 下会误中 Git 自带工具）。
    mingw = os.environ.get("AME_MINGW_BIN", "")
    if mingw.startswith("/"):  # Git Bash POSIX 路径 → Windows 路径
        w = subprocess.run(["cygpath", "-w", mingw], capture_output=True, text=True)
        if w.returncode == 0:
            mingw = w.stdout.strip()
    if mingw:
        env["PATH"] = mingw + os.pathsep + env.get("PATH", "")
    cmd = [str(EXE), "--dataset", "locomo", "--data", str(DATA),
           "--question-ids", str(SAMPLE_IDS), "--store", "lmem",
           "--dump", str(dump), "--cand-dump", str(cands),
           "--out", str(OUT / f"diag_{tag}.json")]
    if not post_p3:  # P3a 规范口径 = P2 管线（P3b 通道关闭，逐比特等价）
        cmd += ["--prox-window", "0", "--no-expand"]
    t0 = time.time()
    with open(log, "w", encoding="utf-8") as lf:
        rc = subprocess.run(cmd, cwd=ROOT, stdout=lf, stderr=lf, env=env).returncode
    if rc != 0:
        sys.exit(f"bench 运行失败 rc={rc}，见 {log}")
    return time.time() - t0, dump, cands


def classify(rows, cand_rows, gold_texts):
    out = []
    for qid, row in sorted(rows.items()):
        gold = [g for g in (row.get("gold_evidence") or []) if not g.startswith("S:")]
        if not gold:
            continue  # 无金标题不进分类（sample50 中 2 题）
        conv = row["question_id"].split(":")[0]
        gtexts = gold_texts.get(conv, {})
        ovs = [(g, overlap(row.get("question", ""), gtexts[g]))
               for g in gold if g in gtexts]
        max_ov = max((o for _, o in ovs), default=None)

        cands = (cand_rows.get(qid) or {}).get("candidates") or []
        pool_dias = {c["dia_id"] for c in cands if c.get("dia_id")}
        pool_hit = sorted(set(gold) & pool_dias)
        pool_rank = min((c["rank"] for c in cands if c.get("dia_id") in set(gold)),
                        default=None)
        final_ev = set(row.get("evidence_dia_ids") or [])
        final_hit = sorted(set(gold) & final_ev)

        if final_hit:
            cls = "HIT"
        elif pool_hit:
            cls = "B"
        elif max_ov is None:
            cls = "C"  # 金标轮文本缺失（数据侧），无法判 A，保守归 C 并 footnote
        elif max_ov < A_OVERLAP_THRESHOLD:
            cls = "A"
        else:
            cls = "C"
        out.append({
            "question_id": qid, "cat": row.get("category", 0), "cls": cls,
            "gold": gold, "final_hit": final_hit, "pool_hit": pool_hit,
            "pool_rank": pool_rank, "max_overlap": max_ov,
            "n_candidates": len(cands),
        })
    return out


def fmt(x, nd=3):
    return "-" if x is None else f"{x:.{nd}f}"


def p3c_trigger(items):
    """P3c 触发判定（D1 预固定）：cat1 miss 中 A 类占比 ≥ 40%。"""
    grp = [it for it in items if it["cat"] == 1]
    n_a = sum(1 for it in grp if it["cls"] == "A")
    miss = sum(1 for it in grp if it["cls"] in ("A", "B", "C"))
    return miss > 0 and n_a / miss >= 0.4


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # Windows 控制台 GBK 兜底
    ap = argparse.ArgumentParser()
    ap.add_argument("--post-p3", action="store_true",
                    help="P3b 落地后的观测口径（只落 runs/ 明细，不写 docs 报告）")
    args = ap.parse_args()
    if not EXE.exists():
        sys.exit(f"缺少 {EXE}，先 cmake --build build-win")
    elapsed, dump, cands = run_bench(args.post_p3)
    rows = load_ndjson(dump)
    cand_rows = load_ndjson(cands)
    gold_texts = load_gold_texts()
    items = classify(rows, cand_rows, gold_texts)

    tag = "postp3" if args.post_p3 else "p2mode"
    (OUT / f"miss_taxonomy_{tag}.json").write_text(
        json.dumps({"items": items, "A_threshold": A_OVERLAP_THRESHOLD,
                    "p3c_trigger": p3c_trigger(items)},
                   ensure_ascii=False, indent=1),
        encoding="utf-8")
    if args.post_p3:
        print(f"[done] post-P3 观测明细 {OUT.relative_to(ROOT)}/miss_taxonomy_{tag}.json")
        return
    write_report(items, elapsed, tag)


def write_report(items, elapsed, tag):
    # ---- 分类分布表 ----
    cats = sorted({it["cat"] for it in items})
    lines = []
    lines.append("# P3a lmem miss 分类诊断（sample50，D1 预固定判决标准）\n")
    lines.append(f"> 数据：LoCoMo sample50（有金标 n={len(items)}）；管线：--store lmem 基线模式，")
    lines.append("> 候选池 = Mem.recall 输出（k_pool=60，终排 BM25-lite 之前），与正式取证同一口径。")
    lines.append("> **规范口径 = P2 管线**：本报告由最终 commit 加 `--prox-window 0 --no-expand` 复现")
    lines.append("> （P3b 通道关闭时与 P2 逐比特等价，test_lstore ⑦ 验证）——P3a 先行于 P3b，")
    lines.append("> 判决不受 P3b 落地影响；post-P3 观测口径见 miss_taxonomy_postp3.json。")
    lines.append(f"> 判决标准（D1 预固定，先于 P3b/c 实现锁定）：A 改述鸿沟 = 候选池无金标 且")
    lines.append(f"> 问题↔金标轮最大词面重合度 < {A_OVERLAP_THRESHOLD}；B 排序出局 = 候选池有金标")
    lines.append("> 但终排 top-k=10 证据无金标；C 其他（含重合度≥阈值仍无金标 = 召回通道漏召回）。")
    lines.append("> 词面重合度 = |tok(q) ∩ tok(gold)| / |tok(q)|，分词逐字节复刻 memindex_tokenize"
                 "（CJK 二元组 + ASCII 小写词）。\n")
    lines.append("## 分类分布（按 category）\n")
    lines.append("| category | n | HIT | A 改述鸿沟 | B 排序出局 | C 其他 | miss 数 | A 占 miss 比 |")
    lines.append("|---|---|---|---|---|---|---|---|")
    trigger = {}
    for cat in cats:
        grp = [it for it in items if it["cat"] == cat]
        n_hit = sum(1 for it in grp if it["cls"] == "HIT")
        n_a = sum(1 for it in grp if it["cls"] == "A")
        n_b = sum(1 for it in grp if it["cls"] == "B")
        n_c = sum(1 for it in grp if it["cls"] == "C")
        miss = n_a + n_b + n_c
        share = f"{n_a / miss:.3f}" if miss else "-"
        trigger[cat] = (n_a, miss)
        lines.append(f"| cat{cat} | {len(grp)} | {n_hit} | {n_a} | {n_b} | {n_c} | {miss} | {share} |")
    tot_hit = sum(1 for it in items if it["cls"] == "HIT")
    tot = {"A": 0, "B": 0, "C": 0}
    for it in items:
        if it["cls"] in tot:
            tot[it["cls"]] += 1
    miss = tot["A"] + tot["B"] + tot["C"]
    lines.append(f"| **总体** | {len(items)} | {tot_hit} | {tot['A']} | {tot['B']} | {tot['C']} "
                 f"| {miss} | {tot['A'] / miss:.3f} |")
    lines.append("")

    a1, miss1 = trigger.get(1, (0, 0))
    verdict = p3c_trigger(items)
    lines.append("## P3c 触发判定（依据：cat1 miss 中 A 类占比 ≥ 40%）\n")
    lines.append(f"- cat1：A={a1} / miss={miss1}，占比 {a1 / miss1:.3f}" if miss1
                 else "- cat1：无 miss（占比不适用）")
    lines.append(f"- **判定：{'触发，实施 n-gram 软相似通道（P3c）' if verdict else '不触发，不实施 P3c'}**\n")

    lines.append("## 逐题分类表\n")
    lines.append("| question_id | cat | class | pool_rank | max_overlap | 金标(final∩) | 候选池命中 |")
    lines.append("|---|---|---|---|---|---|---|")
    for it in items:
        lines.append(f"| {it['question_id']} | {it['cat']} | {it['cls']} | "
                     f"{'-' if it['pool_rank'] is None else it['pool_rank']} | "
                     f"{fmt(it['max_overlap'])} | "
                     f"{','.join(it['final_hit']) or '-'} | "
                     f"{','.join(it['pool_hit']) or '-'} |")
    lines.append("")
    lines.append("## 运行信息\n")
    lines.append(f"- exe: {EXE.relative_to(ROOT)}；口径 tag={tag}；耗时 {elapsed:.1f}s")
    lines.append(f"- 产物：{OUT.relative_to(ROOT)}/diag_dump_{tag}.ndjson、candidates_{tag}.ndjson、"
                 f"miss_taxonomy_{tag}.json")
    lines.append(f"- 复跑：`python bench/scripts/bench_diagnose_misses.py`（P3a 规范口径）\n")

    REPORT.write_text("\n".join(lines), encoding="utf-8")
    print("\n".join(lines))
    print(f"\n[done] 报告 {REPORT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
