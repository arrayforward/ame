#!/usr/bin/env python3
# 词组合推理（WMD 式）spike：bge token 向量 + 贪心最佳配平 → wmd_cos 特征。
#
# 表示选择（披露）：token 向量 = bge-small-en-v1.5 最后一层 hidden state 中
# 该 token 的上下文向量（非特殊 token；不对 token 做额外均值——句向量已经是
# 全 token 均值，token 层用原始上下文向量。bge 的 token 表示质量一般——
# MLM 预训练的 token 级表示非其训练目标，预期管理见报告）。
# 配平（披露）：两侧分词 → 去停用词（30 词表，两侧同口径）→ 各截 ≤30 token
# → 贪心最佳配平（每轮取余下词对余弦最大者）→
#   wmd_cos = (Σ配对cos − 0.2·未配数) / max(n_q, n_c)
# （贪心近似，非精确 OT——运输问题最优解的贪心界见报告蓝方）
#
# 判决（预注册）：cheap+wmd vs cheap 重放覆盖率 Δ ≥ +0.02（两次复跑均值口径
# 由重放确定性替代——wmd 管线确定性，重放零噪声）→ 成立进引擎集成；
# <+0.02 → 证伪归档。附带：wmd_cos vs 教师排序 Kendall tau、vs svec_cos 相关。
import argparse
import hashlib
import json
import random
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "bench" / "scripts"))
import cheap_rerank_train as crt
import svec_eval as se

STOP = set("""the a an and or but if then of at by for with about into through during
to from in on as is are was were be been being it its this that these those
you your i me my we our they their he him his she her what which who whom when
where why how do does did done doing have has had having will would can could
should than so such not no yes""".split())


class TokenVecCache:
    """token 向量磁盘缓存：tok/{sha1(text)}.{npy,json}（tokens 名 + 向量 [n,384]）。"""

    def __init__(self, dir_path, model_tag):
        self.dir = Path(dir_path)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.tag = model_tag
        self.hits = self.misses = 0

    def _f(self, text):
        h = hashlib.sha1(f"{self.tag}|tok|{text}".encode()).hexdigest()[:24]
        return self.dir / h

    def get(self, text):
        f = self._f(text)
        if f.with_suffix(".npy").exists():
            self.hits += 1
            return json.load(open(f.with_suffix(".json"))), \
                np.load(f.with_suffix(".npy"))
        self.misses += 1
        return None

    def put(self, text, tokens, vecs):
        f = self._f(text)
        json.dump(tokens, open(f.with_suffix(".json"), "w"))
        np.save(f.with_suffix(".npy"), vecs.astype(np.float32))


def tokenize(emb, text):
    """MiniBertTokenizer 分词（去 CLS/SEP）→ token 名列表。"""
    ids = emb.tok.encode(text)[1:-1]
    inv = {i: t for t, i in emb.tok.vocab.items()}
    return [inv.get(i, "[UNK]") for i in ids]


def content_tokens(emb, text, cap=30):
    toks = [t for t in tokenize(emb, text) if t.lower() not in STOP and not t.startswith("[")
            and any(ch.isalnum() for ch in t)]
    return toks[:cap]


def wmd_cos(vq, vc):
    """贪心最佳配平（披露公式见文件头）。vq/vc: [n,384] 已归一侧范数。"""
    nq, nc = len(vq), len(vc)
    if not nq or not nc:
        return 0.0
    sim = vq @ vc.T
    aq = set(range(nq))
    ac = set(range(nc))
    paired = 0.0
    while aq and ac:
        i = max(aq)
        # 在剩余中找全局最大对
        bi, bj, bv = -1, -1, -2.0
        for ii in aq:
            row = sim[ii]
            for jj in ac:
                if row[jj] > bv:
                    bv, bi, bj = row[jj], ii, jj
        paired += bv
        aq.discard(bi)
        ac.discard(bj)
    unpaired = len(aq) + len(ac)
    return (paired - 0.2 * unpaired) / max(nq, nc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pools", default="bench/runs/mechanisms/pools50.ndjson")
    ap.add_argument("--log", nargs="+", default=["bench/runs/mechanisms/m12_cal.log"])
    ap.add_argument("--model-dir", default="bench/models/bge-small-en-v1.5")
    ap.add_argument("--out-dir", default="bench/runs/wmd")
    args = ap.parse_args()

    pools = [json.loads(l) for l in open(ROOT / args.pools, encoding="utf-8")]
    labels = crt.load_labels([str(ROOT / p) for p in args.log])
    out_dir = ROOT / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    emb = se.Embedder(str(ROOT / args.model_dir))
    tvc = TokenVecCache(ROOT / "bench/models/cache/tok", Path(args.model_dir).name)

    def vec_of(text):
        got = tvc.get(text)
        if got is None:
            toks = tokenize(emb, text)
            ids = emb.tok.encode(text)
            import onnxruntime as ort
            m = len(ids)
            feeds = {"input_ids": np.array([ids], dtype=np.int64),
                     "attention_mask": np.ones((1, m), dtype=np.int64)}
            if "token_type_ids" in emb.input_names:
                feeds["token_type_ids"] = np.zeros((1, m), dtype=np.int64)
            hidden = emb.sess.run(None, feeds)[0][0]  # [t, dim]
            vecs = hidden[1:-1]  # 去 CLS/SEP（披露口径）
            vecs = vecs / np.maximum(np.linalg.norm(vecs, axis=1, keepdims=True), 1e-12)
            tvc.put(text, toks, vecs)
            return toks, vecs
        return got

    # ---- wmd_cos 全量（缓存复用）----
    texts = sorted({p["text"] for r in pools for p in r["pool"]} |
                   {r["question"] for r in pools})
    t0 = {}
    for i, t in enumerate(texts):
        toks, vecs = vec_of(t)
        keep = [k for k, tok in enumerate(toks) if tok.lower() not in STOP
                and not tok.startswith("[") and any(ch.isalnum() for ch in tok)][:30]
        t0[t] = vecs[keep] if keep else np.zeros((0, 384), dtype=np.float32)
        if (i + 1) % 500 == 0:
            print(f"[wmd] token 向量 {i + 1}/{len(texts)}", flush=True)
    print(f"[wmd] 缓存命中 {tvc.hits} 新算 {tvc.misses}")

    def wmd(q_text, c_text):
        return wmd_cos(t0[q_text], t0[c_text])

    # ---- 训练对：cheap 11 特征 vs cheap+wmd 12 特征（同折对照）----
    import svec_eval  # noqa
    pairs = []
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
        pw = [crt.text_words(p["text"]) for p in pool]
        order_bm = sorted(range(n), key=lambda i: -bm25[i])
        order_fu = sorted(range(n), key=lambda i: -fus[i])
        rank_bm = {i: r_ for r_, i in enumerate(order_bm)}
        rank_fu = {i: r_ for r_, i in enumerate(order_fu)}
        max_bm = max(bm25) if max(bm25) > 0 else 1.0

        def mm(vals, i):
            lo, hi = min(vals), max(vals)
            return (vals[i] - lo) / (hi - lo) if hi > lo else 0.0

        for i, p in enumerate(pool):
            d = p["dia_id"]
            if d in lab["sel"]:
                y = 1
            elif d in lab["cands"]:
                y = 0
            else:
                continue
            wc = wmd(r["question"], p["text"])
            x = [bm25[i], 1.0 - rank_bm[i] / (n - 1), mm(fus, i),
                 mm([p2["graph"] for p2 in pool], i),
                 mm([p2["ctx"] for p2 in pool], i),
                 mm([p2["time"] for p2 in pool], i),
                 1.0 - rank_fu[i] / (n - 1), 0.0,
                 mm([-t for t in tss], i), bm25[i] / max_bm,
                 (sum(1 for t in q_ents if t in pw[i]) / len(q_ents)) if q_ents else 0.0,
                 wc]
            pairs.append((x, y, qid))
    n_pos = sum(y for _, y, _ in pairs)
    print(f"[train] 对 {len(pairs)}（正例 {n_pos}）题 {len(set(q for _, _, q in pairs))}")

    # ---- 同折 5-fold CV：base(11) vs wmd(12) ----
    qids_l = sorted(set(q for _, _, q in pairs))
    random.Random(42).shuffle(qids_l)
    folds = [set(qids_l[i::5]) for i in range(5)]
    res = {"base": [], "wmd": []}
    for tq in folds:
        tr = [(x, y, q) for x, y, q in pairs if q not in tq]
        te = [(x, y, q) for x, y, q in pairs if q in tq]
        for tag, sl in (("base", slice(0, 11)), ("wmd", slice(0, 12))):
            w, b = crt.train([(x[sl], y, q) for x, y, q in tr])
            sc = [crt.sigmoid(sum(wi * xi for wi, xi in zip(w, x[sl])) + b) for x, _, _ in te]
            res[tag].append(crt.auc(list(zip(sc, [y for _, y, _ in te]))))
    print(f"[cv:base] AUC {sum(res['base'])/5:.3f} [{', '.join(f'{v:.3f}' for v in res['base'])}]")
    print(f"[cv:wmd ] AUC {sum(res['wmd'])/5:.3f} [{', '.join(f'{v:.3f}' for v in res['wmd'])}]")

    # ---- 端到端复刻重放：cheap vs cheap+wmd（题内 minmax 口径，确定性）----
    import math
    base_model = json.load(open(ROOT / "bench/runs/mechanisms/cheap_model_v2.json", encoding="utf-8"))

    def replay(model, use_wmd):
        covs, ah = [], 0
        for r in pools:
            pool = r["pool"]
            n = len(pool)
            cs_ = [p["bm25"] for p in pool]
            fus = [p["fusion"] for p in pool]
            gra = [p["graph"] for p in pool]
            ctx = [p["ctx"] for p in pool]
            tim = [p["time"] for p in pool]
            tss = [p["ts"] for p in pool]
            q_ents = crt.question_entity_tokens(r["question"])
            pw = [crt.text_words(p["text"]) for p in pool]
            ob = sorted(range(n), key=lambda i: -cs_[i])
            of_ = sorted(range(n), key=lambda i: -fus[i])
            rb = {i: r_ for r_, i in enumerate(ob)}
            rf = {i: r_ for r_, i in enumerate(of_)}
            max_bm = max(cs_) if max(cs_) > 0 else 1.0

            def mm(vals, i):
                lo, hi = min(vals), max(vals)
                return (vals[i] - lo) / (hi - lo) if hi > lo else 0.0

            wmd_raw = [wmd(r["question"], p["text"]) for p in pool]
            wmd_mm = [mm(wmd_raw, i) for i in range(n)]
            scored = []
            for i in range(n):
                ew = pw[i]
                ent_ov = (sum(1 for t in q_ents if t in ew) / len(q_ents)) if q_ents else 0.0
                feats = {"bm25": cs_[i], "bm25_rank": 1.0 - rb[i] / (n - 1),
                         "fusion": mm(fus, i), "graph": mm(gra, i), "ctx": mm(ctx, i),
                         "time": mm(tim, i), "fusion_rank": 1.0 - rf[i] / (n - 1),
                         "is_entity": 0.0, "recency": mm([-t for t in tss], i),
                         "bm25_share": cs_[i] / max_bm, "ent_overlap": ent_ov}
                if use_wmd:
                    feats["wmd_cos"] = wmd_mm[i]
                z = model.get("bias", 0.0)
                for k, wt in zip(model["features"], model["weights"]):
                    z += wt * feats.get(k, 0.0)
                z = max(-30.0, min(30.0, z))
                scored.append((1.0 / (1.0 + math.exp(-z)), i))
            scored.sort(key=lambda t: -t[0])
            top10 = {pool[i]["dia_id"] for _, i in scored[:10]}
            c = len(set(r["gold"]) & top10) / len(r["gold"]) if r["gold"] else None
            if c is not None:
                covs.append(c)
                ah += bool(c)
        return sum(covs) / len(covs), ah, len(covs)

    # 全量训练 cheap+wmd 模型 → 重放
    w, b = crt.train([(x[:12], y, q) for x, y, q in pairs])
    wmd_model = {"features": crt.FEATURES + ["wmd_cos"], "weights": w, "bias": b,
                 "meta": {"cv_auc": sum(res["wmd"]) / 5}}
    cb, ab_, nb = replay(base_model, False)
    cw, aw, _ = replay(wmd_model, True)
    print(f"[replay] cheap {cb:.3f} (any-hit {ab_}/{nb}) vs cheap+wmd {cw:.3f} "
          f"(any-hit {aw}/{nb}) Δ={cw - cb:+.3f}")
    print("[wmd模型权重]", {k: round(v, 3) for k, v in zip(wmd_model["features"], w)},
          "bias", round(b, 3))

    # ---- 附带测量：tau vs 教师 + 与 svec_cos 相关 ----
    import re
    rx = re.compile(r"\[llm-rerank\]\s+(\S+?):\s+LLM 置顶 .*?\| 选中: (.*?) \| 候选: (.*)$")
    llm_sel = {}
    for lp in args.log:
        for line in open(ROOT / lp, encoding="utf-8", errors="replace"):
            m = rx.search(line)
            if m and m.group(1) not in llm_sel:
                llm_sel[m.group(1)] = [d for d in m.group(2).split(",") if d]
    # svec_cos 从既有缓存重建（问题向量·证据向量）
    cache = se.EmbedCache(ROOT / "bench/models/cache",
                          Path(args.model_dir).name + "-model_quantized")
    qs = {r["question_id"]: r["question"] for r in pools}
    tau_rows, corr_rows = [], []
    for r in pools:
        qid = r["question_id"]
        qk = se.QUERY_INSTRUCTION + r["question"]
        qv = cache.get("q", qk)
        if qv is None:
            qv = emb.embed_batch([qk], 1)[0]
            cache.put("q", qk, qv)
        wmd_raw = [wmd(r["question"], p["text"]) for p in r["pool"]]
        svec_raw = []
        for p in r["pool"]:
            pv = cache.get("p", p["text"])
            if pv is None:
                pv = emb.embed_batch([p["text"]], 1)[0]
                cache.put("p", p["text"], pv)
            svec_raw.append(float(np.dot(qv, pv)))
        corr_rows.append(np.corrcoef(wmd_raw, svec_raw)[0, 1])
        sel = [d for d in llm_sel.get(qid, []) if d in {p["dia_id"] for p in r["pool"]}]
        if len(sel) >= 2:
            wmd_order = [p["dia_id"] for p in sorted(r["pool"], key=lambda p: -wmd(
                r["question"], p["text"]))]
            wmd_sub = [d for d in wmd_order if d in set(sel)]
            tau_rows.append((qid, se.tau_b(wmd_sub, sel), se.tau_b(
                [p["dia_id"] for p in sorted(r["pool"], key=lambda p: -svec_raw[
                    r["pool"].index(p)]) if p["dia_id"] in set(sel)], sel)))
    taus_w = [t[1] for t in tau_rows if t[1] == t[1]]
    taus_s = [t[2] for t in tau_rows if t[2] == t[2]]
    corr = [c for c in corr_rows if c == c]
    print(f"[tau] wmd vs 教师选中集 {len(taus_w)} 题 平均 "
          f"{sum(taus_w)/len(taus_w):+.3f}（svec 参照 {sum(taus_s)/len(taus_s):+.3f}）")
    print(f"[corr] wmd_cos vs svec_cos 逐题 Pearson 平均 {sum(corr)/len(corr):+.3f}（n={len(corr)}）")

    with open(out_dir / "wmd_results.json", "w", encoding="utf-8") as f:
        json.dump({"cv_base_auc": sum(res["base"]) / 5, "cv_wmd_auc": sum(res["wmd"]) / 5,
                   "replay_base": cb, "replay_wmd": cw, "delta": cw - cb,
                   "model": wmd_model, "tau_wmd": sum(taus_w) / len(taus_w),
                   "tau_svec": sum(taus_s) / len(taus_s),
                   "corr_wmd_svec": sum(corr) / len(corr)}, f, ensure_ascii=False, indent=1,
              default=lambda o: float(o))
    print(f"[out] {out_dir / 'wmd_results.json'}")


if __name__ == "__main__":
    main()
