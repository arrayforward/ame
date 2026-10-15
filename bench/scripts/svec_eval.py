#!/usr/bin/env python3
# 句向量（sentence embedding）离线评估：bge-small-en-v1.5 作 cheap 档追加特征 svec_cos。
#
# 回答：端侧小模型能把 cheap 档从 0.54~0.57 带抬到多少，值不值 ~100MB APK。
# 本任务只做测量不集成：C++ 引擎不动，端到端用 Python 复刻 cheap 档重排语义
# （cheap_score_pool 逐行对齐，见 replay_cheap），并以 cheap_model_v2 复算覆盖率
# 与引擎实测带（0.573/0.536）校准。
#
# 测量内容：
#   1) svec_cos（问题向量·证据向量余弦，题内 minmax）追加进 11 特征 → 同折 5-fold
#      GroupKFold 重训，与 11 特征基线同标签同折对照（AUC / 正例 top-10 召回）；
#   2) 端到端复刻重放：cheap_model_v2 vs cheap_svec 模型，覆盖率/any-hit/逐题 Δ；
#   3) Kendall tau：svec 排序 vs LLM 全池教师排序（m12_cal 日志选中序），B 类 6 题；
#   4) 端侧集成账素材：embed 批量吞吐、单查询延迟、模型磁盘体积（int8/fp32）。
#
# 纪律声明：
#   - svec 是独立第三方模型（BAAI bge-small-en-v1.5），与 label 来源（LLM 选中）
#     无任何同源关系 → 无标签泄漏；embed 按文本 hash 磁盘缓存，可复跑零成本。
#   - LoCoMo 为英文语料：en 模型为主测量对象；zh 模型仅端侧尺寸参照（不跑推理）。
#   - 模型权重不入 git（bench/models/ 已 gitignore）。
#
# 用法: python bench/scripts/svec_eval.py \
#         --pools bench/runs/mechanisms/pools50.ndjson \
#         --log bench/runs/mechanisms/m12_cal.log \
#         --baseline-model bench/runs/mechanisms/cheap_model_v2.json \
#         --model-dir bench/models/bge-small-en-v1.5 \
#         --out-dir bench/runs/svec
import argparse
import hashlib
import json
import math
import random
import sys
import time
import unicodedata
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "bench" / "scripts"))
import cheap_rerank_train as crt  # noqa: E402  复用训练框架（train/auc/topk_recall/load_labels/...）

SVEC_FEATURE = "svec_cos"
# bge v1.5 检索查询指令（仅加在问题侧；证据侧不加）——官方推荐口径。
QUERY_INSTRUCTION = "Represent this sentence for searching relevant passages: "
B_CLASS6 = ["conv-26:q10", "conv-26:q101", "conv-26:q154", "conv-26:q155",
            "conv-26:q34", "conv-26:q50"]


# ---------------------------------------------------------------- tokenizer
# 最小 BertTokenizer（WordPiece），从 Xenova 导出的 tokenizer.json 读 vocab。
# 覆盖 BertNormalizer(lowercase+去重音+控字符清理+中文字按字切)/BertPreTokenizer
# (空白与标点切分)/WordPiece 贪心最长匹配；截断到 max_len-2 加 [CLS]/[SEP]。
class MiniBertTokenizer:
    def __init__(self, tokenizer_json, max_len=512):
        d = json.load(open(tokenizer_json, encoding="utf-8"))
        model = d["model"]
        assert model["type"] == "WordPiece", f"expect WordPiece, got {model['type']}"
        self.vocab = model["vocab"]
        self.max_word = model.get("max_input_chars_per_word", 100)
        self.cls = self.vocab["[CLS]"]
        self.sep = self.vocab["[SEP]"]
        self.unk = self.vocab["[UNK]"]
        self.max_len = max_len

    @staticmethod
    def _normalize(text):
        # BertNormalizer(clean_text)：仅去控制字符(Cc)/U+FFFD + lowercase + NFD 去重音；
        # 标点与空白全部保留（切分是 PreTokenizer 的职责）。
        out = []
        for ch in text:
            cat = unicodedata.category(ch)
            if cat == "Cc" or ch == "�":
                continue
            out.append(ch)
        s = unicodedata.normalize("NFD", "".join(out)).lower()
        return "".join(c for c in s if unicodedata.category(c) != "Mn")

    def _pre_tokenize(self, s):
        # BertPreTokenizer：空白与标点（P*）切分，标点自成 token；中文按字切。
        toks, cur = [], ""
        for ch in s:
            cat = unicodedata.category(ch)
            if cat.startswith("P") or cat.startswith("Z"):
                if cur:
                    toks.append(cur)
                    cur = ""
                if cat.startswith("P"):
                    toks.append(ch)
            elif unicodedata.category(ch).startswith("Lo") and not ch.isascii():
                if cur:
                    toks.append(cur)
                    cur = ""
                toks.append(ch)
            else:
                cur += ch
        if cur:
            toks.append(cur)
        return toks

    def _wordpiece(self, word):
        if len(word) > self.max_word:
            return ["[UNK]"]
        chars = list(word)
        sub, start = [], 0
        while start < len(chars):
            end, cur = len(chars), None
            while start < end:
                piece = "".join(chars[start:end])
                if start > 0:
                    piece = "##" + piece
                if piece in self.vocab:
                    cur = piece
                    break
                end -= 1
            if cur is None:
                return ["[UNK]"]
            sub.append(cur)
            start = end
        return sub

    def encode(self, text):
        ids = [self.cls]
        for tok in self._pre_tokenize(self._normalize(text)):
            ids.extend(self.vocab[p] for p in self._wordpiece(tok))
        ids.append(self.sep)
        return ids[: self.max_len]


# ---------------------------------------------------------------- embedder
class Embedder:
    def __init__(self, model_dir):
        import onnxruntime as ort

        model_dir = Path(model_dir)
        cand = [model_dir / "onnx" / "model_quantized.onnx", model_dir / "onnx" / "model.onnx"]
        path = next((p for p in cand if p.exists()), None)
        if path is None:
            raise SystemExit(f"模型不可得: {model_dir} 下无 onnx/model_quantized.onnx 或 model.onnx")
        self.path = path
        self.tok = MiniBertTokenizer(model_dir / "tokenizer.json")
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = max(1, (os_cpu_count() - 1))
        self.sess = ort.InferenceSession(str(path), opts, providers=["CPUExecutionProvider"])
        self.input_names = [i.name for i in self.sess.get_inputs()]

    def embed_batch(self, texts, batch_size=64):
        """return L2 归一化句向量 [n, dim]（bge 口径：last_hidden_state 按 mask 均值池化）。"""
        out = np.zeros((len(texts), self.sess.get_outputs()[0].shape[-1]), dtype=np.float32)
        for lo in range(0, len(texts), batch_size):
            chunk = [self.tok.encode(t) for t in texts[lo:lo + batch_size]]
            m = max(len(c) for c in chunk)
            ids = np.zeros((len(chunk), m), dtype=np.int64)
            mask = np.zeros((len(chunk), m), dtype=np.int64)
            tti = np.zeros((len(chunk), m), dtype=np.int64)
            for i, c in enumerate(chunk):
                ids[i, : len(c)] = c
                mask[i, : len(c)] = 1
            feeds = {"input_ids": ids, "attention_mask": mask}
            if "token_type_ids" in self.input_names:
                feeds["token_type_ids"] = tti
            hidden = self.sess.run(None, feeds)[0]  # [b, t, dim] fp32
            maskf = mask.astype(np.float32)[..., None]
            emb = (hidden * maskf).sum(axis=1) / np.maximum(maskf.sum(axis=1), 1e-9)
            norms = np.maximum(np.linalg.norm(emb, axis=1, keepdims=True), 1e-12)
            out[lo:lo + len(chunk)] = emb / norms
        return out


def os_cpu_count():
    import os
    return os.cpu_count() or 4


class EmbedCache:
    """文本 hash 磁盘缓存：{sha1(model|kind|text)}.npy → 单位向量。"""

    def __init__(self, dir_path, model_tag):
        self.dir = Path(dir_path)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.tag = model_tag
        self.hits = self.misses = 0

    def _file(self, kind, text):
        h = hashlib.sha1(f"{self.tag}|{kind}|{text}".encode("utf-8")).hexdigest()[:24]
        return self.dir / f"{h}.npy"

    def get(self, kind, text):
        f = self._file(kind, text)
        if f.exists():
            self.hits += 1
            return np.load(f)
        self.misses += 1
        return None

    def put(self, kind, text, vec):
        np.save(self._file(kind, text), vec.astype(np.float32))


# ---------------------------------------------------------------- 特征/训练
def build_pairs_svec(pools, labels, vecs):
    """cheap_rerank_train.build_pairs 同口径 + svec_cos（第 12 特征，题内 minmax）。
    vecs: {(qid_or kind, text) -> vec}；问题向量键 ('q', qid)，证据键 ('p', text)。"""
    pairs, stats = [], {"match": 0, "label_only_skip": 0, "conservative_zero": 0}
    for r in pools:
        qid = r["question_id"]
        if qid not in labels:
            continue
        lab = labels[qid]
        pool = r["pool"]
        n = len(pool)
        if n < 2:
            continue
        bm25 = [p["bm25"] for p in pool]
        fus = [p["fusion"] for p in pool]
        tss = [p["ts"] for p in pool]
        q_ents = crt.question_entity_tokens(r["question"])
        pool_words = [crt.text_words(p["text"]) for p in pool]
        order_bm = sorted(range(n), key=lambda i: -bm25[i])
        order_fu = sorted(range(n), key=lambda i: -fus[i])
        rank_bm = {i: r_ for r_, i in enumerate(order_bm)}
        rank_fu = {i: r_ for r_, i in enumerate(order_fu)}

        def mm(vals, i):
            lo, hi = min(vals), max(vals)
            return (vals[i] - lo) / (hi - lo) if hi > lo else 0.0

        max_bm = max(bm25) if max(bm25) > 0 else 1.0
        qv = vecs[("q", qid)]
        cos_raw = [float(np.dot(qv, vecs[("p", p["text"])])) for p in pool]

        def mm_cos(i):
            lo, hi = min(cos_raw), max(cos_raw)
            return (cos_raw[i] - lo) / (hi - lo) if hi > lo else 0.0

        for i, p in enumerate(pool):
            d = p["dia_id"]
            if d in lab["sel"]:
                y = 1
                stats["match"] += 1
            elif d in lab["cands"]:
                y = 0
                stats["conservative_zero"] += 1
            else:
                stats["label_only_skip"] += 1
                continue
            cos = mm_cos(i)  # 与引擎复刻同口径：题内 minmax（训练/服务一致）
            x = [bm25[i],
                 1.0 - rank_bm[i] / (n - 1),
                 mm(fus, i),
                 mm([p2["graph"] for p2 in pool], i),
                 mm([p2["ctx"] for p2 in pool], i),
                 mm([p2["time"] for p2 in pool], i),
                 1.0 - rank_fu[i] / (n - 1),
                 0.0,  # is_entity：标准池恒 0（同 cheap v1/v2）
                 mm([-t for t in tss], i),
                 bm25[i] / max_bm,
                 (sum(1 for t in q_ents if t in pool_words[i]) / len(q_ents)) if q_ents else 0.0,
                 cos]
            pairs.append((x, y, qid))
    return pairs, stats


# ---------------------------------------------------------------- 端到端复刻重放
def replay_cheap(model, question, pool, vecs=None, qid=None):
    """逐行复刻 ingest_answer.cpp cheap_score_pool + 终排前 key 排序（默认 final_mix=关
    → key=cs 降序 stable）+ apply_selection（置顶 top-k、其余原序）。返回 (top_k dia 序, 全序)。"""
    n = len(pool)
    cs = [p["bm25"] for p in pool]  # 导出口径 bm25 == 引擎 cs（oracle_ceiling 同源）
    order0 = sorted(range(n), key=lambda i: -cs[i])  # stable = 引擎 key 降序
    evs = [pool[i] for i in order0]

    def mm(v, lo, hi):
        return (v - lo) / (hi - lo) if hi > lo else 0.0

    fus = [e["fusion"] for e in evs]
    gra = [e["graph"] for e in evs]
    ctx = [e["ctx"] for e in evs]
    tim = [e["time"] for e in evs]
    rec = [-e["ts"] for e in evs]
    spans = {k: (min(v), max(v)) for k, v in
             (("fusion", fus), ("graph", gra), ("ctx", ctx), ("time", tim), ("recency", rec))}

    def norm(name, v):
        lo, hi = spans[name]
        return mm(v, lo, hi)

    ord_bm = sorted(range(n), key=lambda i: -cs[i])
    ord_fu = sorted(range(n), key=lambda i: -fus[i])
    rank_bm = [0.0] * n
    rank_fu = [0.0] * n
    for r_, i in enumerate(ord_bm):
        rank_bm[i] = 1.0 - r_ / (n - 1)
    for r_, i in enumerate(ord_fu):
        rank_fu[i] = 1.0 - r_ / (n - 1)
    bm_max = max(cs) if max(cs) > 0 else 1.0
    q_ents = crt.question_entity_tokens(question)
    svec_raw = None
    if vecs is not None:
        qv = vecs[("q", qid)]
        svec_raw = [float(np.dot(qv, vecs[("p", e["text"])])) for e in evs]
    feats, weights = model["features"], model["weights"]
    bias = model.get("bias", 0.0)
    scored = []
    for i in range(n):
        ew = crt.text_words(evs[i]["text"])
        ent_ov = (sum(1 for t in q_ents if t in ew) / len(q_ents)) if q_ents else 0.0
        x = {"bm25": cs[i], "bm25_rank": rank_bm[i],
             "fusion": norm("fusion", fus[i]), "graph": norm("graph", gra[i]),
             "ctx": norm("ctx", ctx[i]), "time": norm("time", tim[i]),
             "fusion_rank": rank_fu[i], "is_entity": 0.0,
             "recency": norm("recency", rec[i]), "bm25_share": cs[i] / bm_max,
             "ent_overlap": ent_ov}
        if svec_raw is not None and SVEC_FEATURE in feats:
            x[SVEC_FEATURE] = mm(svec_raw[i], min(svec_raw), max(svec_raw))
        z = bias + sum(weights[k] * x[feats[k]] for k in range(len(feats)))
        z = max(-30.0, min(30.0, z))
        scored.append((1.0 / (1.0 + math.exp(-z)), i))
    scored.sort(key=lambda t: -t[0])  # stable = 引擎 stable_sort 降序
    order = [i for _, i in scored]
    return [evs[i]["dia_id"] for i in order[:10]], [evs[i]["dia_id"] for i in order]


def coverage_of(gold, dias):
    if not gold:
        return None
    return len(set(gold) & set(dias)) / len(gold)


# ---------------------------------------------------------------- Kendall tau-b
def tau_b(order_a, order_b):
    """order_a/order_b: 同一元素集的两种排序（列表）。tau-b（并列计 0.5）。"""
    pos_b = {e: i for i, e in enumerate(order_b)}
    a = [e for e in order_a if e in pos_b]
    conc = disc = tie_a = 0
    for i in range(len(a)):
        for j in range(i + 1, len(a)):
            di = pos_b[a[i]] - pos_b[a[j]]
            if di < 0:
                conc += 1
            elif di > 0:
                disc += 1
            else:
                tie_a += 1
    n0 = len(a) * (len(a) - 1) / 2
    if n0 == 0:
        return float("nan")
    return (conc - disc) / n0  # 简化 tau-b：本场景两种排序内均无并列，tau-a=tau-b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pools", default="bench/runs/mechanisms/pools50.ndjson")
    ap.add_argument("--log", nargs="+", default=["bench/runs/mechanisms/m12_cal.log"])
    ap.add_argument("--baseline-model", default="bench/runs/mechanisms/cheap_model_v2.json")
    ap.add_argument("--model-dir", default="bench/models/bge-small-en-v1.5")
    ap.add_argument("--out-dir", default="bench/runs/svec")
    ap.add_argument("--batch-size", type=int, default=64)
    ap.add_argument("--k", type=int, default=10)
    a = ap.parse_args()

    pools = [json.loads(l) for l in open(a.pools, encoding="utf-8")]
    out_dir = Path(a.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    labels = crt.load_labels(a.log)
    base_model = json.load(open(a.baseline_model, encoding="utf-8"))

    # ---------- 1. embed（磁盘缓存，文本 hash） ----------
    emb = Embedder(a.model_dir)
    cache = EmbedCache(out_dir / "embed_cache", emb.path.stem)
    texts_p = sorted({p["text"] for r in pools for p in r["pool"]})
    qids = [r["question_id"] for r in pools]
    vecs = {}

    t0 = time.perf_counter()
    miss_p = [t for t in texts_p if cache.get("p", t) is None]
    if miss_p:
        for lo in range(0, len(miss_p), a.batch_size):
            for t, v in zip(miss_p[lo:lo + a.batch_size],
                            emb.embed_batch(miss_p[lo:lo + a.batch_size], a.batch_size)):
                cache.put("p", t, v)
    t_pass = time.perf_counter() - t0
    for t in texts_p:
        vecs[("p", t)] = cache.get("p", t)

    t0 = time.perf_counter()
    miss_q = [(r["question_id"], r["question"]) for r in pools
              if cache.get("q", QUERY_INSTRUCTION + r["question"]) is None]
    for qid, q in miss_q:
        cache.put("q", QUERY_INSTRUCTION + q, emb.embed_batch([QUERY_INSTRUCTION + q], 1)[0])
    t_query = time.perf_counter() - t0
    for r in pools:
        vecs[("q", r["question_id"])] = cache.get("q", QUERY_INSTRUCTION + r["question"])

    n_texts = len(texts_p) + len(pools)
    print(f"[embed] 模型 {emb.path.name}  缓存命中 {cache.hits} / 新算 {cache.misses}")
    print(f"[embed] 证据 {len(texts_p)} 条（去重）+ 问题 {len(pools)} 题；"
          f"证据批量均速 {len(texts_p) / max(t_pass, 1e-9):.0f} 条/s"
          f"（{t_pass:.1f}s），问题单条均延迟 {t_query / len(pools) * 1000:.1f} ms/题"
          f"（含缓存命中题则更低；每查询端侧成本 = 1 次问题 embed + 60 次点积）")

    # ---------- 2. 训练对 + 同折 5-fold 对照 ----------
    pairs, stats = build_pairs_svec(pools, labels, vecs)
    n_pos = sum(y for _, y, _ in pairs)
    print(f"[train] 训练对 {len(pairs)}（正例 {n_pos} = {n_pos / len(pairs):.1%}）"
          f" 题数 {len(set(q for _, _, q in pairs))} 标注: {stats}")

    qids_l = sorted(set(q for _, _, q in pairs))
    random.Random(42).shuffle(qids_l)
    folds = [set(qids_l[i::5]) for i in range(5)]
    res = {"base": {"auc": [], "rec": []}, "svec": {"auc": [], "rec": []}}
    for test_q in folds:
        tr = [(x, y, q) for x, y, q in pairs if q not in test_q]
        te = [(x, y, q) for x, y, q in pairs if q in test_q]
        for tag, sl in (("base", slice(0, 11)), ("svec", slice(0, 12))):
            w, b = crt.train([(x[sl], y, q) for x, y, q in tr])
            scores = [crt.sigmoid(sum(wi * xi for wi, xi in zip(w, x[sl])) + b)
                      for x, _, _ in te]
            res[tag]["auc"].append(crt.auc(list(zip(scores, [y for _, y, _ in te]))))
            res[tag]["rec"].append(crt.topk_recall(te, scores))
    for tag in ("base", "svec"):
        print(f"[cv:{tag}] AUC = {sum(res[tag]['auc']) / 5:.3f} "
              f"[{', '.join(f'{v:.3f}' for v in res[tag]['auc'])}]  "
              f"top10召回 = {sum(res[tag]['rec']) / 5:.3f} "
              f"[{', '.join(f'{v:.3f}' for v in res[tag]['rec'])}]")

    # 全量训练 svec 模型 → 持久化
    w, b = crt.train([(x[:12], y, q) for x, y, q in pairs])
    svec_model = {"features": crt.FEATURES + [SVEC_FEATURE], "weights": w, "bias": b,
                  "meta": {"train_pairs": len(pairs), "pos": n_pos,
                           "cv_auc": sum(res["svec"]["auc"]) / 5,
                           "cv_top10_recall": sum(res["svec"]["rec"]) / 5,
                           "labels": "m12 LLM top-10",
                           "svec": "bge-small-en-v1.5 cosine, query instruction, "
                                   "minmax within pool"}}
    model_path = out_dir / "cheap_model_svec.json"
    with open(model_path, "w", encoding="utf-8") as f:
        json.dump(svec_model, f, ensure_ascii=False, indent=1)
    print(f"[model] {model_path}")
    print(f"[model] 权重:", {k: round(v, 3) for k, v in zip(svec_model["features"], w)},
          "bias:", round(b, 3))

    # ---------- 3. 端到端复刻重放 ----------
    rows = {}
    for tag, mdl, use_vec in (("bm25", None, None), ("base", base_model, None),
                              ("svec", svec_model, vecs)):
        covs, ah = [], 0
        per_q = {}
        for r in pools:
            if mdl is None:
                cs = [p["bm25"] for p in r["pool"]]
                order = sorted(range(len(cs)), key=lambda i: -cs[i])
                top10 = [r["pool"][i]["dia_id"] for i in order[: a.k]]
            else:
                top10, _ = replay_cheap(mdl, r["question"], r["pool"],
                                        vecs if use_vec is not None else None,
                                        r["question_id"])
            c = coverage_of(r["gold"], top10)
            per_q[r["question_id"]] = {"top10": top10, "cov": c}
            if c is not None:
                covs.append(c)
                ah += bool(c > 0)
        rows[tag] = per_q
        print(f"[replay:{tag}] 覆盖率 = {sum(covs) / len(covs):.3f}  any-hit {ah}/{len(covs)}"
              f"（n={len(covs)} 题，零 LLM 复刻口径，标准池无实体通道并入）")

    rescued, lost = [], []
    for qid in rows["base"]:
        cb, cs_ = rows["base"][qid]["cov"], rows["svec"][qid]["cov"]
        if cb is None or cs_ is None:
            continue
        if cs_ > cb:
            rescued.append((qid, cb, cs_))
        elif cs_ < cb:
            lost.append((qid, cb, cs_))
    print(f"[replay:Δ svec-base] 救回 {len(rescued)} {[q for q, _, _ in rescued]}  "
          f"丢失 {len(lost)} {[q for q, _, _ in lost]}")
    for qid, cb, cs_ in rescued + lost:
        print(f"    {qid}: {cb:.3f} -> {cs_:.3f}")

    with open(out_dir / "sim_per_question.json", "w", encoding="utf-8") as f:
        json.dump({tag: {q: v["top10"] for q, v in per.items()} for tag, per in rows.items()},
                  f, ensure_ascii=False, indent=1)

    # ---------- 4. Kendall tau：svec vs LLM 全池教师（B 类 6 题） ----------
    import re
    rx = re.compile(r"\[llm-rerank\]\s+(\S+?):\s+LLM 置顶 .*?\| 选中: (.*?) \| 候选: (.*)$")
    llm_sel = {}
    for lp in a.log:
        for line in open(lp, encoding="utf-8", errors="replace"):
            m = rx.search(line)
            if m:
                llm_sel.setdefault(m.group(1), [])
                if not llm_sel[m.group(1)]:
                    llm_sel[m.group(1)] = [d for d in m.group(2).split(",") if d]
    tau_rows = []
    for qid in B_CLASS6:
        r = next(x for x in pools if x["question_id"] == qid)
        qv = vecs[("q", qid)]
        pool = r["pool"]
        svec_order = [p["dia_id"] for p in
                      sorted(pool, key=lambda p: -float(np.dot(qv, vecs[("p", p["text"])])))]
        sel = [d for d in llm_sel.get(qid, []) if d in {p["dia_id"] for p in pool}]
        # 变体 A：限定 LLM 选中集（教师 top-10 内部序 vs svec 在该子集上的序）
        tau_a = tau_b([d for d in svec_order if d in set(sel)], sel) if len(sel) >= 2 else float("nan")
        # 变体 B：全池——LLM 选中置顶（按响应序）+ 未选中按 svec 序补齐（并列中性）
        rest = [d for d in svec_order if d not in set(sel)]
        tau_b_full = tau_b(svec_order, sel + rest) if len(sel) >= 2 else float("nan")
        # 参照：BM25(cs) 序 / 融合序 对同一 LLM 全池序
        cs_order = [p["dia_id"] for p in sorted(pool, key=lambda p: -p["bm25"])]
        fu_order = [p["dia_id"] for p in sorted(pool, key=lambda p: -p["fusion"])]
        tau_bm = tau_b(cs_order, sel + [d for d in cs_order if d not in set(sel)]) \
            if len(sel) >= 2 else float("nan")
        tau_fu = tau_b(fu_order, sel + [d for d in fu_order if d not in set(sel)]) \
            if len(sel) >= 2 else float("nan")
        gold_rank = None
        if r["gold"]:
            g0 = r["gold"][0]
            gold_rank = (next((i + 1 for i, d in enumerate(svec_order) if d == g0), None),
                         next((i + 1 for i, d in enumerate(sel) if d == g0), None))
        tau_rows.append((qid, tau_a, tau_b_full, tau_bm, tau_fu, gold_rank))
    print("[tau:B类6题] qid | τ(svec,LLM选中集) | τ(svec,LLM全池) | "
          "τ(bm25,LLM全池) | τ(fusion,LLM全池) | 金标svec秩/LLM秩")
    for qid, ta, tb_, tbm, tfu, gr in tau_rows:
        f = lambda v: "  n/a" if v != v else f"{v:+.3f}"  # noqa: E731
        print(f"    {qid}: {f(ta)} {f(tb_)} {f(tbm)} {f(tfu)}  {gr}")
    vals = [row[2] for row in tau_rows if row[2] == row[2]]
    print(f"[tau] svec vs LLM 全池 平均 tau = {sum(vals) / len(vals):+.3f}（n={len(vals)}）")

    # ---------- 5. 端侧集成账素材 ----------
    def du(path):
        return Path(path).stat().st_size / 1e6 if Path(path).exists() else 0.0
    md = Path(a.model_dir)
    print(f"[size] int8 onnx {du(md / 'onnx/model_quantized.onnx'):.1f} MB  "
          f"fp32 onnx {du(md / 'onnx/model.onnx'):.1f} MB  "
          f"tokenizer.json {du(md / 'tokenizer.json'):.2f} MB  "
          f"(zh 参照: {du(ROOT / 'bench/models/bge-small-zh-v1.5/onnx/model_quantized.onnx'):.1f} MB int8)")


if __name__ == "__main__":
    main()
