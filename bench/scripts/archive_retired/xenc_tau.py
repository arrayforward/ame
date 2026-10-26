#!/usr/bin/env python3
# xenc tau 测量：cross-encoder 分数 vs LLM 教师选中序（B 类口径同 svec/wmd）。
# 独立直连 scorer 的 pair 推理（复用 xenc_scorer 的 PairCache/encode_pair），
# 输入 pools50 的问题+候选，输出逐题 tau（wmd/svec 同口径：教师选中集内部序）。
import json
import re
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "bench" / "scripts"))
import svec_eval as se  # noqa: E402
from xenc_scorer import PairCache, encode_pair, load_tokenizer  # noqa: E402

pools = [json.loads(l) for l in open(ROOT / "bench/runs/mechanisms/pools50.ndjson", encoding="utf-8")]
sess = ort.InferenceSession(str(ROOT / "bench/models/bge-reranker-base/onnx/model_quantized.onnx"),
                            providers=["CPUExecutionProvider"])
tok = load_tokenizer(str(ROOT / "bench/models/bge-reranker-base"))
cache = PairCache("bge-reranker-base")
HAS_TTI = "token_type_ids" in [i.name for i in sess.get_inputs()]

labels = {}
rx = re.compile(r"\[llm-rerank\]\s+(\S+?):\s+LLM 置顶 .*?\| 选中: (.*?) \| 候选: (.*)$")
for line in open(ROOT / "bench/runs/mechanisms/m12_cal.log", encoding="utf-8", errors="replace"):
    m = rx.search(line)
    if m and m.group(1) not in labels:
        labels[m.group(1)] = [d for d in m.group(2).split(",") if d]


def score_batch(q_text, docs):
    q_ids = tok.encode(q_text)[1:-1]
    out = [None] * len(docs)
    miss = []
    for i, d in enumerate(docs):
        s = cache.get(q_text, d)
        if s is None:
            miss.append(i)
        else:
            out[i] = s
    for lo in range(0, len(miss), 32):
        chunk = miss[lo:lo + 32]
        enc = [encode_pair(tok, q_ids, tok.encode(docs[i])[1:-1]) for i in chunk]
        m = max(len(e[0]) for e in enc)
        ids = np.zeros((len(chunk), m), dtype=np.int64)
        tti = np.zeros((len(chunk), m), dtype=np.int64)
        mask = np.zeros((len(chunk), m), dtype=np.int64)
        for r, (ei, et) in enumerate(enc):
            ids[r, :len(ei)] = ei
            tti[r, :len(ei)] = et
            mask[r, :len(ei)] = 1
        feeds = {"input_ids": ids, "attention_mask": mask}
        if HAS_TTI:
            feeds["token_type_ids"] = tti
        res = sess.run(None, feeds)[0].reshape(-1)
        for k, i in enumerate(chunk):
            out[i] = float(res[k])
            cache.put(q_text, docs[i], out[i])
    return out


taus, gold_ranks = [], []
for r in pools:
    qid = r["question_id"]
    docs = [p["text"] for p in r["pool"]]
    sc = score_batch(r["question"], docs)
    order = [r["pool"][i]["dia_id"] for i in sorted(range(len(sc)), key=lambda i: -sc[i])]
    sel = [d for d in labels.get(qid, []) if d in {p["dia_id"] for p in r["pool"]}]
    if len(sel) >= 2:
        sub = [d for d in order if d in set(sel)]
        taus.append(se.tau_b(sub, sel))
    if r["gold"]:
        g0 = r["gold"][0]
        rank = next((i + 1 for i, d in enumerate(order) if d == g0), None)
        gold_ranks.append((qid, rank))

vals = [t for t in taus if t == t]
print(f"[tau] xenc vs 教师选中集 {len(vals)} 题 平均 {sum(vals)/len(vals):+.3f}"
      f"（svec 同口径参照 +0.216）")
print(f"[gold-rank] 金标在 xenc 序中的位次（前 15）："
      f"{[f'{q}:{g}' for q, g in gold_ranks[:15]]}")
with open(ROOT / "bench/runs/wmd/xenc_tau.json", "w", encoding="utf-8") as f:
    json.dump({"tau_mean": sum(vals) / len(vals), "n": len(vals),
               "gold_ranks": gold_ranks}, f, ensure_ascii=False, indent=1)
