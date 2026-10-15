#!/usr/bin/env python3
# 便宜近似 v1：特征逻辑重排器——监督提取 + 训练 + 5-fold 交叉验证 + 模型持久化。
# 监督来源：pools50.ndjson（oracle_ceiling pools50 模式：标准池 + 引擎现成特征）
#           + m12 运行日志（LLM top-10 选中 dia_id = 标签；NONE 降级题无标签，剔除）。
# 特征泄漏纪律：特征只用引擎信号（BM25/融合分量/秩/recency），不含任何 LLM 产物。
# 隔离：sample50 全为单 case（conv-26），case 级隔离不可得 → 按 question_id 分组
#       （GroupKFold），防同题候选泄漏。
# 用法: python bench/scripts/cheap_rerank_train.py \
#         --pools bench/runs/mechanisms/pools50.ndjson --log bench/runs/mechanisms/m12_cal.log \
#         --out bench/runs/mechanisms/cheap_model.json
import argparse
import json
import math
import random
import re

FEATURES = ["bm25", "bm25_rank", "fusion", "graph", "ctx", "time", "fusion_rank",
            "is_entity", "recency", "bm25_share", "ent_overlap"]


def question_entity_tokens(question):
    """问题专名 token（大写非句首 len≥2，小写化）——与 ingest_answer.cpp 同口径。"""
    out, cur, sent_start = [], "", True

    def flush():
        nonlocal cur, sent_start
        if len(cur) >= 2 and not sent_start and cur[0].isupper():
            out.append(cur.lower())
        cur = ""

    for ch in question + "\x00":
        if ch.isascii() and ch.isalnum():
            cur += ch
        else:
            flush()
            sent_start = ch in ".!?\x00"
        if ch == " ":
            sent_start = False
    return out


def text_words(text):
    """ASCII 小写切词 len≥3（content_words 同口径）。"""
    words, cur = set(), ""
    for ch in text:
        if ch.isascii() and ch.isalnum():
            cur += ch.lower()
        else:
            if len(cur) >= 3:
                words.add(cur)
            cur = ""
    if len(cur) >= 3:
        words.add(cur)
    return words
B_GOLD = {"conv-26:q10": "D3:13", "conv-26:q101": "D5:8",
          "conv-26:q154": "D2:12", "conv-26:q155": "D2:12"}


def load_labels(log_paths):
    """m12 运行日志（可多份，取并集）→ {qid: {"sel": set(dia), "cands": set(dia)}}。
    跨运行 uid 噪声使单轮候选集/选中集合有边界翻转：任一运行选中即正例
    （降低假阴性）；被某轮展示但从未选中才记负例；从未展示的候选不入对。"""
    labels = {}
    rx = re.compile(r"\[llm-rerank\]\s+(\S+?):\s+LLM 置顶 \d+/\d+ 条 \| 榜首内容分 [\d.]+ \| "
                    r"选中: (.*?) \| 候选: (.*)$")
    for log_path in log_paths:
        for line in open(log_path, encoding="utf-8", errors="replace"):
            m = rx.search(line)
            if m:
                ent = labels.setdefault(m.group(1), {"sel": set(), "cands": set()})
                ent["sel"] |= set(filter(None, m.group(2).split(",")))
                ent["cands"] |= set(filter(None, m.group(3).split(",")))
    return labels


def build_pairs(pools_path, labels):
    """(x, y, qid) 训练对；跨运行 dia 匹配策略见报告蓝方。"""
    pairs, stats = [], {"match": 0, "label_only_skip": 0, "conservative_zero": 0}
    for line in open(pools_path, encoding="utf-8"):
        r = json.loads(line)
        qid = r["question_id"]
        if qid not in labels:
            continue  # NONE 降级 / 未触达题：无标签，剔除
        lab = labels[qid]
        pool = r["pool"]
        n = len(pool)
        if n < 2:
            continue
        bm25 = [p["bm25"] for p in pool]
        fus = [p["fusion"] for p in pool]
        tss = [p["ts"] for p in pool]
        q_ents = question_entity_tokens(r["question"])
        pool_words = [text_words(p["text"]) for p in pool]
        order_bm = sorted(range(n), key=lambda i: -bm25[i])
        order_fu = sorted(range(n), key=lambda i: -fus[i])
        rank_bm = {i: r_ for r_, i in enumerate(order_bm)}
        rank_fu = {i: r_ for r_, i in enumerate(order_fu)}

        def mm(vals, i):
            lo, hi = min(vals), max(vals)
            return (vals[i] - lo) / (hi - lo) if hi > lo else 0.0

        max_bm = max(bm25) if max(bm25) > 0 else 1.0
        for i, p in enumerate(pool):
            d = p["dia_id"]
            if d in lab["sel"]:
                y = 1  # 任一运行选中 → 正例
                stats["match"] += 1
            elif d in lab["cands"]:
                y = 0  # 展示过但从未选中 → 负例
                stats["conservative_zero"] += 1
            else:
                stats["label_only_skip"] += 1
                continue  # 从未被任何一轮 LLM 看见 → 不入对（防假负例）
            x = [bm25[i],
                 1.0 - rank_bm[i] / (n - 1),
                 mm(fus, i),
                 mm([p2["graph"] for p2 in pool], i),
                 mm([p2["ctx"] for p2 in pool], i),
                 mm([p2["time"] for p2 in pool], i),
                 1.0 - rank_fu[i] / (n - 1),
                 0.0,  # is_entity：标准池恒 0（合并候选未入训练池，见报告）
                 mm([-t for t in tss], i),
                 bm25[i] / max_bm,
                 (sum(1 for t in q_ents if t in pool_words[i]) / len(q_ents)) if q_ents else 0.0]
            pairs.append((x, y, qid))
    return pairs, stats


def sigmoid(z):
    return 1.0 / (1.0 + math.exp(-max(-30.0, min(30.0, z))))


def train(pairs, epochs=300, lr=0.5, l2=1e-4):
    """纯 Python 批量梯度下降逻辑回归（零依赖，仿 GateModel 风格）。"""
    d = len(pairs[0][0])
    w = [0.0] * d
    b = 0.0
    n = len(pairs)
    for _ in range(epochs):
        g = [0.0] * d
        gb = 0.0
        for x, y, _ in pairs:
            e = sigmoid(sum(wi * xi for wi, xi in zip(w, x)) + b) - y
            for j in range(d):
                g[j] += e * x[j]
            gb += e
        for j in range(d):
            w[j] -= lr * (g[j] / n + l2 * w[j])
        b -= lr * gb / n
    return w, b


def auc(scored):
    """rank-based AUC：scored = [(score, y)]"""
    pos = [(s, y) for s, y in scored if y == 1]
    neg = [(s, y) for s, y in scored if y == 0]
    if not pos or not neg:
        return float("nan")
    wins = 0.0
    for ps, _ in pos:
        for ns, _ in neg:
            wins += 1.0 if ps > ns else (0.5 if ps == ns else 0.0)
    return wins / (len(pos) * len(neg))


def topk_recall(pairs, scores, k=10):
    """逐题：模型 top-k 命中 LLM 选中（正例）的比例（宏平均）"""
    byq = {}
    for (x, y, qid), s in zip(pairs, scores):
        byq.setdefault(qid, []).append((s, y))
    recalls = []
    for qid, lst in byq.items():
        pos = sum(y for _, y in lst)
        if pos == 0:
            continue
        lst.sort(key=lambda t: -t[0])
        hits = sum(y for _, y in lst[:k])
        recalls.append(hits / pos)
    return sum(recalls) / len(recalls) if recalls else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pools", required=True)
    ap.add_argument("--log", required=True, nargs="+")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    labels = load_labels(a.log)
    pairs, stats = build_pairs(a.pools, labels)
    n_pos = sum(y for _, y, _ in pairs)
    print(f"训练对: {len(pairs)}（正例 {n_pos} = {n_pos/len(pairs):.1%}）"
          f" 题数 {len(set(q for _,_,q in pairs))} 标注: {stats}（label_only_skip=从未被 LLM 看见而剔除）")

    # 5-fold GroupKFold by question_id
    qids = sorted(set(q for _, _, q in pairs))
    random.Random(42).shuffle(qids)
    folds = [set(qids[i::5]) for i in range(5)]
    aucs, recalls, b_hits = [], [], []
    for fi, test_q in enumerate(folds):
        tr = [(x, y, q) for x, y, q in pairs if q not in test_q]
        te = [(x, y, q) for x, y, q in pairs if q in test_q]
        w, b = train(tr)
        scores = [sigmoid(sum(wi * xi for wi, xi in zip(w, x)) + b) for x, _, _ in te]
        aucs.append(auc(list(zip(scores, [y for _, y, _ in te]))))
        recalls.append(topk_recall(te, scores))
        # B 类金标进模型 top-10（held-out 题内）
        for x, y, qid in te:
            if qid in B_GOLD:
                pass  # 由逐题排序判：下方统一算
        byq = {}
        for (x, y, qid), s in zip(te, scores):
            byq.setdefault(qid, []).append((s, y, x))
        hit = 0
        for qid, lst in byq.items():
            if qid not in B_GOLD:
                continue
            lst.sort(key=lambda t: -t[0])
            # 金标 dia → 该题内找：特征无 dia，用 pools 反查太绕；近似用 y 不行。
            # B 类验收放在端到端 --cheap-rerank 运行，此处仅留 AUC/召回。
        b_hits.append(hit)
    print(f"5-fold CV: AUC = {sum(aucs)/len(aucs):.3f} "
          f"[{', '.join(f'{a_:.3f}' for a_ in aucs)}]")
    print(f"5-fold CV: 正例 top-10 召回(宏平均) = {sum(recalls)/len(recalls):.3f} "
          f"[{', '.join(f'{r:.3f}' for r in recalls)}]")

    # 全量训练 → 持久化
    w, b = train(pairs)
    model = {"features": FEATURES, "weights": w, "bias": b,
             "meta": {"train_pairs": len(pairs), "pos": n_pos,
                      "cv_auc": sum(aucs) / len(aucs),
                      "cv_top10_recall": sum(recalls) / len(recalls),
                      "labels": "m12 LLM top-10", "note": "is_entity 训练恒0"}}
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(model, f, ensure_ascii=False, indent=1)
    print(f"模型已写 {a.out}")
    print("权重:", {k: round(v, 3) for k, v in zip(FEATURES, w)}, "bias:", round(b, 3))


if __name__ == "__main__":
    main()
