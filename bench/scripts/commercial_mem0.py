#!/usr/bin/env python3
# 商业对标 · Mem0 OSS 臂（venv python 运行；LLM/embedding 全走本地 openai_shim：
# DeepSeek 经磁盘缓存转发 + bge-small 本地嵌入——DeepSeek 无 embedding API 的替代，配置即本文件）。
#
# 写入：逐轮 add(messages=[user 轮文本], metadata={dia_id})（Mem0 内部 LLM 事实提取）
# 查询：mem0.search(question, limit=10)
# 覆盖率映射（Mem0 记忆是事实级非轮级，粒度差异如实披露）：
#   宽松 = top-10 记忆带 dia_id 元数据的并集（含跨轮合并记忆挂的全部 dia_id）
#   严格 = 仅计"单轮来源"记忆（metadata 恰挂 1 个 dia_id）的 dia_id
# 用法: bench/venv-commercial/Scripts/python.exe bench/scripts/commercial_mem0.py
import argparse
import json
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
OUT = ROOT / "bench/runs/commercial"
QDRANT = OUT / "qdrant"
SHIM = "http://localhost:8399/v1"
MODEL_CFG = json.load(open(ROOT / "bench/config/deepseek.local.json", encoding="utf-8"))

sys.path.insert(0, str(ROOT / "bench" / "scripts"))


def load_turns():
    data = json.load(open(ROOT / "bench/data/locomo10.json", encoding="utf-8"))
    case = next(c for c in data if c["sample_id"] == "conv-26")
    conv = case["conversation"]
    sess = sorted((k for k in conv if k.startswith("session_") and not k.endswith("date_time")),
                  key=lambda s: int(s.split("_")[1]))
    return [(t["dia_id"], t["speaker"], t["text"]) for s in sess for t in conv[s]]


def load_questions():
    out = []
    for line in open(ROOT / "bench/runs/svec_eng/llm.ndjson", encoding="utf-8"):
        r = json.loads(line)
        if r.get("gold_evidence"):
            out.append({"qid": r["question_id"], "question": r["question"],
                        "gold": r["gold_evidence"]})
    return out


def main():
    from mem0 import Memory

    ap = argparse.ArgumentParser()
    ap.add_argument("--search-only", action="store_true",
                    help="跳过写入，直接查既有 qdrant 库（解析调试/复跑查询用）")
    a0 = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    if QDRANT.exists() and not a0.search_only:
        shutil.rmtree(QDRANT)  # 全量重写，防重复记忆污染

    config = {
        "llm": {"provider": "openai",
                "config": {"model": MODEL_CFG.get("model", "deepseek-chat"),
                           "api_key": "sk-shim", "openai_base_url": SHIM}},
        "embedder": {"provider": "openai",
                     "config": {"model": "bge-small-local", "api_key": "sk-shim",
                                "openai_base_url": SHIM, "embedding_dims": 384}},
        "vector_store": {"provider": "qdrant",
                         "config": {"collection_name": "commercial", "path": str(QDRANT),
                                    "embedding_model_dims": 384}},
        "version": "v1.1",
    }
    m = Memory.from_config(config)

    turns = load_turns()
    qs = load_questions()
    print(f"[mem0] turns={len(turns)} questions={len(qs)}")

    t0 = time.perf_counter()
    fails = 0
    if a0.search_only:
        print("[mem0] --search-only：跳过写入")
    for i, (dia, speaker, text) in enumerate(turns):
        if a0.search_only:
            break
        try:
            m.add([{"role": "user", "content": text}], user_id="conv-26",
                  metadata={"dia_id": dia})
        except Exception as e:
            fails += 1
            print(f"[mem0] add fail {dia}: {e}", file=sys.stderr)
        if (i + 1) % 50 == 0:
            print(f"[mem0] add {i + 1}/{len(turns)} ({time.perf_counter() - t0:.0f}s)", flush=True)
    write_s = time.perf_counter() - t0
    print(f"[mem0] 写入完成 {len(turns) - fails}/{len(turns)} 条，{write_s:.0f}s，失败 {fails}")

    per_q = []
    lat = []
    dbg = False
    for q in qs:
        t1 = time.perf_counter()
        res = m.search(q["question"], limit=10, filters={"user_id": "conv-26"})
        lat.append((time.perf_counter() - t1) * 1000)
        # mem0 2.2.1 search 返回 dict（含 results/memories 键）或 list，统一归一为 list。
        if isinstance(res, dict):
            res = res.get("results") or res.get("memories") or []
        if not dbg:
            print(f"[mem0] search 返回结构样例: {json.dumps(res, ensure_ascii=False, default=str)[:300]}")
            dbg = True
        dias_loose, dias_strict = set(), set()
        for item in res:
            if isinstance(item, dict) and item.get("memory"):  # 事实级记忆文本（粒度差异见报告）
                pass
            md = item.get("metadata") or {} if isinstance(item, dict) else {}
            dia = md.get("dia_id") if isinstance(md, dict) else None
            if isinstance(dia, list):
                dias_loose |= set(dia)
                if len(dia) == 1:
                    dias_strict |= set(dia)
            elif dia:
                dias_loose.add(dia)
                dias_strict.add(dia)
        per_q.append({"qid": q["qid"], "question": q["question"], "gold": q["gold"],
                      "loose": sorted(dias_loose), "strict": sorted(dias_strict),
                      "n_mem": len(res)})

    def cov(field):
        cs = [len(set(p["gold"]) & set(p[field])) / len(p["gold"]) for p in per_q]
        ah = sum(bool(c) for c in cs)
        return sum(cs) / len(cs), ah

    cl, ahl = cov("loose")
    cs_, ahs = cov("strict")
    lat_sorted = sorted(lat)
    result = {"arm": "mem0-oss", "coverage_loose": round(cl, 3), "any_hit_loose": f"{ahl}/48",
              "coverage_strict": round(cs_, 3), "any_hit_strict": f"{ahs}/48",
              "write_s": round(write_s, 1), "add_fails": fails,
              "query_ms_median": round(lat_sorted[len(lat) // 2], 1),
              "query_ms_p90": round(lat_sorted[int(len(lat) * 0.9)], 1)}
    print(json.dumps(result, ensure_ascii=False, indent=1))
    with open(OUT / "mem0_results.json", "w", encoding="utf-8") as f:
        json.dump({"summary": result, "per_question": per_q}, f, ensure_ascii=False, indent=1)
    print(f"[out] {OUT / 'mem0_results.json'}")


if __name__ == "__main__":
    main()
