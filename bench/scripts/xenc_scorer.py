#!/usr/bin/env python3
# cross-encoder 精排外挂打分器（--rerank xenc 的每题一次 spawn，协议同 svec_scorer）。
#
# 协议：--pool <json>（{"question":..., "texts":[...]}）→ stdout {"scores":[...]}
#   （每候选一个相关性分，越高越相关）；stderr 诊断 JSON。
# 模型：bge-reranker-base（Xenova ONNX int8，bench/models/）。
#   pair 构造（披露位置偏置口径）：[CLS] q [SEP] d [SEP]，token_type q=0/d=1
#   （BGE reranker 训练惯例）；max_seq 512 截断（q~15+d~60 token，绰绰有余）。
# 得分 = [CLS] 位置的 logits（pair 分类头）。
# 缓存：pair 级 sha1(模型|q|d) → .npy 单分（bench/models/cache/xenc/），复跑零推理。
# 用法: python bench/scripts/xenc_scorer.py --pool /tmp/ame_xenc_pool_XXXX.json
import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "bench" / "scripts"))
import svec_eval as se  # noqa: E402  MiniBertTokenizer（WordPiece 模型用）

MODEL_DIR = ROOT / "bench/models/bge-reranker-base"
CACHE_DIR = ROOT / "bench/models/cache/xenc"
MAX_LEN = 512
BATCH = 32


class MiniUnigramTokenizer:
    """最小 Unigram 分词器（sentencepiece 风格，bge-reranker 系）：
    Metaspace 预切分（空格→▁+前缀▁）+ Viterbi 最大分。normalizer 为 Precompiled
    charsmap（NFKC 类），本实现对英文语料近似为恒等——近似声明见报告蓝方。
    特殊 token：<s>=0 <pad>=1 </s>=2 <unk>=3。"""

    def __init__(self, tokenizer_json):
        d = json.load(open(tokenizer_json, encoding="utf-8"))
        assert d["model"]["type"] == "Unigram"
        self.vocab = {t: i for i, (t, _s) in enumerate(d["model"]["vocab"])}
        self.score = {t: s for t, s in d["model"]["vocab"]}
        self.unk = d["model"].get("unk_id", 3)

    def encode(self, text):
        s = "▁" + text.replace(" ", "▁")
        n = len(s)
        best = [None] * (n + 1)  # (累积分, 末 token 长)
        best[0] = (0.0, 0)
        for i in range(n):
            if best[i] is None:
                continue
            for L in range(1, min(24, n - i) + 1):
                piece = s[i:i + L]
                if piece in self.vocab:
                    sc = best[i][0] + self.score[piece]
                    if best[i + L] is None or sc > best[i + L][0]:
                        best[i + L] = (sc, L)
        ids = []
        i = n
        spans = []
        while i > 0:
            if best[i] is None:  # 无覆盖：单字 unk
                spans.append((1, i - 1))
                i -= 1
                continue
            L = best[i][1]
            spans.append((L, i - L))
            i -= L
        spans.reverse()
        for L, st in spans:
            piece = s[st:st + L]
            ids.append(self.vocab.get(piece, self.unk))
        return [0] + ids + [2]  # <s> seq </s>


def load_tokenizer(model_dir):
    tj = Path(model_dir) / "tokenizer.json"
    d = json.load(open(tj, encoding="utf-8"))
    if d["model"]["type"] == "Unigram":
        return MiniUnigramTokenizer(str(tj))
    return se.MiniBertTokenizer(str(tj))


class PairCache:
    def __init__(self, tag):
        self.dir = CACHE_DIR
        self.dir.mkdir(parents=True, exist_ok=True)
        self.tag = tag
        self.hits = self.misses = 0

    def _f(self, q, d):
        h = hashlib.sha1(f"{self.tag}|{q}|{d}".encode()).hexdigest()[:24]
        return self.dir / f"{h}.npy"

    def get(self, q, d):
        f = self._f(q, d)
        if f.exists():
            self.hits += 1
            return float(np.load(f))
        self.misses += 1
        return None

    def put(self, q, d, v):
        np.save(self._f(q, d), np.float32(v))


def encode_pair(tok, q_ids, d_ids):
    cls_, sep_ = (tok.cls, tok.sep) if hasattr(tok, "cls") else (0, 2)
    ids = [cls_] + q_ids + [sep_] + d_ids + [sep_]
    tti = [0] * (1 + len(q_ids) + 1) + [1] * (len(d_ids) + 1)
    if len(ids) > MAX_LEN:  # 截断 doc 侧保 query（披露：实际 60+15 远低於 512）
        over = len(ids) - MAX_LEN
        ids = ids[:1 + len(q_ids) + 1] + ids[1 + len(q_ids) + 1 + over:]
        tti = tti[:len(ids)]
    return ids, tti


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pool", required=True)
    ap.add_argument("--model-dir", default=str(MODEL_DIR))
    a = ap.parse_args()

    t0 = time.perf_counter()
    data = json.load(open(a.pool, encoding="utf-8"))
    question, texts = data["question"], data["texts"]

    import onnxruntime as ort
    mdir = Path(a.model_dir)
    path = mdir / "onnx/model_quantized.onnx"
    if not path.exists():
        path = mdir / "onnx/model.onnx"
    if not path.exists():
        raise SystemExit(f"模型不可得: {mdir}")
    sess = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
    tok = load_tokenizer(mdir)
    cache = PairCache(mdir.name)

    q_ids = tok.encode(question)[1:-1]
    scores = [None] * len(texts)
    miss_idx = []
    for i, d in enumerate(texts):
        s = cache.get(question, d)
        if s is None:
            miss_idx.append(i)
        else:
            scores[i] = s
    t1 = time.perf_counter()
    for lo in range(0, len(miss_idx), BATCH):
        chunk = miss_idx[lo:lo + BATCH]
        enc = [encode_pair(tok, q_ids, tok.encode(texts[i])[1:-1]) for i in chunk]
        m = max(len(e[0]) for e in enc)
        ids = np.zeros((len(chunk), m), dtype=np.int64)
        tti = np.zeros((len(chunk), m), dtype=np.int64)
        mask = np.zeros((len(chunk), m), dtype=np.int64)
        for r, (e_ids, e_tti) in enumerate(enc):
            ids[r, :len(e_ids)] = e_ids
            tti[r, :len(e_ids)] = e_tti
            mask[r, :len(e_ids)] = 1
        feeds = {"input_ids": ids, "attention_mask": mask}
        if "token_type_ids" in [i.name for i in sess.get_inputs()]:
            feeds["token_type_ids"] = tti
        out = sess.run(None, feeds)[0].reshape(-1)  # [CLS] logits
        for k, i in enumerate(chunk):
            scores[i] = float(out[k])
            cache.put(question, texts[i], scores[i])
    t2 = time.perf_counter()
    print(json.dumps({"scores": [round(s, 6) for s in scores]}))
    print(json.dumps({"load_ms": round((t1 - t0) * 1000, 1),
                      "infer_ms": round((t2 - t1) * 1000, 1),
                      "hits": cache.hits, "misses": cache.misses,
                      "n": len(texts)}), file=sys.stderr)


if __name__ == "__main__":
    main()
