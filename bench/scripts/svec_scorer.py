#!/usr/bin/env python3
# svec 档外挂打分器（--rerank svec 的 cos 提供者；bench 每题候选池 spawn 一次）。
#
# 协议（stdin/stdout JSON，临时文件交互，仿 llm curl 子进程传输先例）：
#   输入  --pool <json 文件>：{"question": "...", "texts": ["...", ...]}
#   输出  stdout 单行：{"cos": [float x N]}（与 texts 等长，原始余弦，未归一）
#   诊断  stderr 单行 JSON：{"load_ms":..., "embed_ms":..., "hits":..., "misses":..., "n":...}
#
# 推理复用 svec_eval.py（同 tokenizer/ONNX/查询指令口径）；embed 磁盘缓存在
# bench/models/cache（文本 sha1，复跑零推理）。任何失败 → 非零退出 + stderr
# traceback，bench 侧检测到后该题降级 cheap 原分（svec_cos 置 0）。
#
# 用法: python bench/scripts/svec_scorer.py --pool /tmp/ame_svec_pool_XXXX.json
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import svec_eval as se  # noqa: E402  MiniBertTokenizer / Embedder / EmbedCache / QUERY_INSTRUCTION

ROOT = Path(__file__).resolve().parent.parent.parent          # ame/
DEFAULT_MODEL_DIR = ROOT / "bench" / "models" / "bge-small-en-v1.5"
CACHE_DIR = ROOT / "bench" / "models" / "cache"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pool", required=True, help="输入 JSON 文件（question + texts）")
    ap.add_argument("--model-dir", default=str(DEFAULT_MODEL_DIR))
    ap.add_argument("--batch-size", type=int, default=64)
    a = ap.parse_args()

    t0 = time.perf_counter()
    data = json.load(open(a.pool, encoding="utf-8"))
    question, texts = data["question"], data["texts"]

    emb = se.Embedder(a.model_dir)
    cache = se.EmbedCache(CACHE_DIR, emb.path.stem)

    # 问题（带 bge 查询指令）+ 候选批量 embed，全部走磁盘缓存。
    qkey = se.QUERY_INSTRUCTION + question
    miss_q = cache.get("q", qkey) is None
    t1 = time.perf_counter()
    qv = cache.get("q", qkey)
    if qv is None:
        qv = emb.embed_batch([qkey], 1)[0]
        cache.put("q", qkey, qv)
    miss = [t for t in texts if cache.get("p", t) is None]
    for lo in range(0, len(miss), a.batch_size):
        chunk = miss[lo:lo + a.batch_size]
        for t, v in zip(chunk, emb.embed_batch(chunk, a.batch_size)):
            cache.put("p", t, v)
    t2 = time.perf_counter()

    import numpy as np
    cos = [float(np.dot(qv, cache.get("p", t))) for t in texts]
    print(json.dumps({"cos": [round(c, 6) for c in cos]}))
    print(json.dumps({
        "load_ms": round((t1 - t0) * 1000, 1),
        "embed_ms": round((t2 - t1) * 1000, 1),
        "hits": cache.hits, "misses": cache.misses + (1 if miss_q else 0),
        "n": len(texts),
    }), file=sys.stderr)


if __name__ == "__main__":
    main()
