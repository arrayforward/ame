#!/usr/bin/env python3
# 行业参考记忆基线（Mem0/Zep 式 RAG 的零-LLM 形态）——与 AME 四档同题同指标正面比。
#
# 三个参照臂（全部 LoCoMo sample50 / conv-26，零 LLM，纯检索）：
#   dense-turn   轮切块：每对话轮 = chunk，bge-small-en-v1.5(int8 onnx) embed → 余弦 top-10
#   dense-window 500-token 滑窗（交叠 50），窗挂覆盖的 dia_id 集合 → 余弦 top-10
#   bm25-turn    轮级词面 BM25（手写倒排 + IDF，k1=1.2 b=0.75 行业默认），top-10
# 可选上限参照臂：dense-turn + bge-large-en-v1.5（下载顺畅才跑）。
#
# 指标与 AME 完全同口径：覆盖率 = |gold dia_ids ∩ 检索 top-10| / |gold|（宏平均，
# n=48）；any-hit 并列。窗臂双口径：主口径 = 严格（金标轮被 top-10 窗完整覆盖），
# 参考口径 = 窗与金标轮有交叠即算（见报告蓝方）。
#
# 公平性声明：参照臂用行业默认直觉配置、不调参到最优；嵌入缓存在
# bench/models/cache（文本 sha1，跨臂/跨 run 复用，复跑零推理）。
#
# 用法: python bench/scripts/ref_rag_baseline.py [--arm all|dense-turn|dense-window|bm25-turn|dense-large]
import argparse
import hashlib
import json
import math
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "bench" / "scripts"))
import svec_eval as se  # noqa: E402  MiniBertTokenizer / Embedder / EmbedCache / QUERY_INSTRUCTION

CACHE_DIR = ROOT / "bench" / "models" / "cache"
CASE_ID = "conv-26"
WINDOW_TOKENS = 500
WINDOW_OVERLAP = 50


# ---------------------------------------------------------------- 语料/问题装载
def load_turns():
    """locomo10.json conv-26 → 有序 [(dia_id, speaker, text)]（文本纯 utterance，
    与 AME 事件文本同口径；不拼 speaker 前缀，参照臂与 AME 特征输入保持一致）。"""
    data = json.load(open(ROOT / "bench" / "data" / "locomo10.json", encoding="utf-8"))
    case = next(c for c in data if c["sample_id"] == CASE_ID)
    conv = case["conversation"]
    sess = sorted((k for k in conv if k.startswith("session_") and not k.endswith("date_time")),
                  key=lambda s: int(s.split("_")[1]))
    turns = []
    for s in sess:
        for t in conv[s]:
            turns.append((t["dia_id"], t["speaker"], t["text"]))
    return turns


def load_questions(dump_path):
    """从 AME judge 臂 dump 取问题与金标（与 AME 判分同一份数据，零歧义）。"""
    out = []
    for line in open(dump_path, encoding="utf-8"):
        r = json.loads(line)
        if not r.get("gold_evidence"):
            continue
        out.append({"qid": r["question_id"], "question": r["question"],
                    "gold": r["gold_evidence"]})
    return out


# ---------------------------------------------------------------- 嵌入工具
def embed_all(emb, cache, texts, kind, batch=64):
    miss = [t for t in texts if cache.get(kind, t) is None]
    for lo in range(0, len(miss), batch):
        chunk = miss[lo:lo + batch]
        for t, v in zip(chunk, emb.embed_batch(chunk, batch)):
            cache.put(kind, t, v)
    return np.stack([cache.get(kind, t) for t in texts])


def embed_queries(emb, cache, questions):
    keys = [se.QUERY_INSTRUCTION + q for q in questions]
    miss = [k for k in keys if cache.get("q", k) is None]
    for k in miss:
        cache.put("q", k, emb.embed_batch([k], 1)[0])
    return np.stack([cache.get("q", k) for k in keys])


# ---------------------------------------------------------------- 词面 BM25（手写，零依赖）
def tokenize(text):
    """AME content_words 同口径：ASCII 小写切词 len≥3。"""
    words, cur = [], ""
    for ch in text:
        if ch.isascii() and ch.isalnum():
            cur += ch.lower()
        else:
            if len(cur) >= 3:
                words.append(cur)
            cur = ""
    if len(cur) >= 3:
        words.append(cur)
    return words


class Bm25:
    def __init__(self, docs_tokens, k1=1.2, b=0.75):
        self.k1, self.b = k1, b
        self.docs = docs_tokens
        self.n = len(docs_tokens)
        self.df = {}
        self.dl = []
        for toks in docs_tokens:
            self.dl.append(len(toks))
            for w in set(toks):
                self.df[w] = self.df.get(w, 0) + 1
        self.avgdl = sum(self.dl) / self.n if self.n else 1.0

    def idf(self, w):
        df = self.df.get(w, 0)
        return math.log(1.0 + (self.n - df + 0.5) / (df + 0.5)) if df else 0.0

    def search(self, query_tokens, topk=10):
        scores = np.zeros(self.n)
        for w in set(query_tokens):
            idf = self.idf(w)
            if idf <= 0:
                continue
            for d, toks in enumerate(self.docs):
                tf = toks.count(w)
                if not tf:
                    continue
                denom = tf + self.k1 * (1 - self.b + self.b * self.dl[d] / self.avgdl)
                scores[d] += idf * tf * (self.k1 + 1) / denom
        return np.argsort(-scores)[:topk]


# ---------------------------------------------------------------- 窗切块
def build_windows(turns, tok):
    """token 流（逐轮 encode 去 CLS/SEP）→ 500/50 滑窗。
    返回 (win_texts, win_dias, win_spans, turn_spans)；turn_spans = 每轮 [起,止)
    在 token 流中的位置（半开区间）。"""
    stream, turn_spans, turn_dias = [], [], []
    for dia, _sp, text in turns:
        ids = tok.encode(text)[1:-1]
        turn_spans.append((len(stream), len(stream) + len(ids)))
        stream.extend(ids)
        turn_dias.append(dia)
    inv = {i: t for t, i in tok.vocab.items()}

    def detok(ids):
        out = ""
        for i in ids:
            p = inv.get(i, "")
            if p.startswith("##"):
                out += p[2:]
            else:
                out += " " + p
        return out.strip()

    step = WINDOW_TOKENS - WINDOW_OVERLAP
    wins = []
    for lo in range(0, max(1, len(stream) - WINDOW_OVERLAP), step):
        hi = min(lo + WINDOW_TOKENS, len(stream))
        ids = stream[lo:hi]
        dias = {turn_dias[t] for t, (a, b) in enumerate(turn_spans) if a < hi and b > lo}
        wins.append((detok(ids), dias, (lo, hi)))
        if hi == len(stream):
            break
    win_texts = [w[0] for w in wins]
    win_dias = [w[1] for w in wins]
    win_spans = [w[2] for w in wins]
    return win_texts, win_dias, win_spans, turn_spans, turn_dias


# ---------------------------------------------------------------- 指标（与 AME 同口径）
def evaluate(gold, retrieved_sets, n_questions):
    covs, ah = [], 0
    for g, ret in zip(gold, retrieved_sets):
        c = len(set(g) & ret) / len(g)
        covs.append(c)
        ah += bool(c > 0)
    return sum(covs) / n_questions, ah


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", default="all",
                    choices=["all", "dense-turn", "dense-window", "bm25-turn", "dense-large"])
    ap.add_argument("--dump", default="bench/runs/svec_eng/llm.ndjson",
                    help="AME judge 臂 dump（取问题与金标）")
    ap.add_argument("--model-dir", default="bench/models/bge-small-en-v1.5")
    ap.add_argument("--large-dir", default="bench/models/bge-large-en-v1.5")
    ap.add_argument("--k", type=int, default=10)
    a = ap.parse_args()

    turns = load_turns()
    qs = load_questions(ROOT / a.dump)
    gold = [q["gold"] for q in qs]
    n = len(qs)
    print(f"[corpus] conv-26 轮次 {len(turns)}；题目 {n}（有金标）")
    assert n == 48, f"题目数 {n} != 48（dump 口径漂移？）"

    results = {}

    if a.arm in ("all", "dense-turn", "dense-window", "dense-large"):
        model_dir = a.large_dir if a.arm == "dense-large" else a.model_dir
        if not (ROOT / model_dir / "onnx" / "model_quantized.onnx").exists():
            print(f"[skip] {model_dir} 模型不可得，臂 {a.arm} 跳过")
        else:
            emb = se.Embedder(str(ROOT / model_dir))
            # 缓存键防碰撞：small/large 的 onnx 文件名同为 model_quantized，
            # 键必须带模型目录名（否则 large 臂会吃到 small 的向量——蓝方 #5）。
            cache = se.EmbedCache(CACHE_DIR, Path(model_dir).name + "-" + emb.path.stem)
            qv = embed_queries(emb, cache, [q["question"] for q in qs])
            if a.arm in ("all", "dense-turn", "dense-large"):
                tag = "dense-large" if a.arm == "dense-large" else "dense-turn"
                tv = embed_all(emb, cache, [t[2] for t in turns], "p")
                sim = qv @ tv.T
                top = np.argsort(-sim, axis=1)[:, : a.k]
                ret = [{turns[i][0] for i in row} for row in top]
                cov, ah = evaluate(gold, ret, n)
                results[tag] = {"coverage": round(cov, 3), "any_hit": f"{ah}/{n}"}
                print(f"[{tag}] 覆盖率={cov:.3f} any-hit={ah}/{n}（模型 {Path(model_dir).name}）")
            if a.arm in ("all", "dense-window"):
                tok = emb.tok
                win_texts, win_dias, win_spans, turn_spans, turn_dias = build_windows(turns, tok)
                wv = embed_all(emb, cache, win_texts, "w")
                sim = qv @ wv.T
                top = np.argsort(-sim, axis=1)[:, : a.k]
                # 主口径（严格）：金标轮须被 top-10 窗完整覆盖（turn span ⊆ win span）
                ret_strict = []
                ret_overlap = []
                for row in top:
                    spans = [win_spans[i] for i in row]
                    s_set, o_set = set(), set()
                    for t, (ta, tb) in enumerate(turn_spans):
                        dia = turn_dias[t]
                        for ws, we in spans:
                            if ws <= ta and tb <= we:
                                s_set.add(dia)
                            if ws < tb and ta < we:
                                o_set.add(dia)
                    ret_strict.append(s_set)
                    ret_overlap.append(o_set)
                cov_s, ah_s = evaluate(gold, ret_strict, n)
                cov_o, ah_o = evaluate(gold, ret_overlap, n)
                results["dense-window"] = {
                    "coverage_strict": round(cov_s, 3), "any_hit_strict": f"{ah_s}/{n}",
                    "coverage_overlap": round(cov_o, 3), "any_hit_overlap": f"{ah_o}/{n}",
                    "n_windows": len(win_texts)}
                print(f"[dense-window] 窗数={len(win_texts)} 严格口径 覆盖率={cov_s:.3f} "
                      f"any-hit={ah_s}/{n}；交叠口径 覆盖率={cov_o:.3f} any-hit={ah_o}/{n}")

    if a.arm in ("all", "bm25-turn"):
        docs = [tokenize(t[2]) for t in turns]
        bm = Bm25(docs)
        ret = []
        for q in qs:
            idx = bm.search(tokenize(q["question"]), a.k)
            ret.append({turns[i][0] for i in idx})
        cov, ah = evaluate(gold, ret, n)
        results["bm25-turn"] = {"coverage": round(cov, 3), "any_hit": f"{ah}/{n}"}
        print(f"[bm25-turn] 覆盖率={cov:.3f} any-hit={ah}/{n}（手写 BM25 k1=1.2 b=0.75）")

    out = ROOT / "bench" / "runs" / "svec_eng" / "ref_rag_results.json"
    prev = json.load(open(out, encoding="utf-8")) if out.exists() else {}
    prev.update(results)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(prev, f, ensure_ascii=False, indent=1)
    print(f"[out] {out}")


if __name__ == "__main__":
    main()
