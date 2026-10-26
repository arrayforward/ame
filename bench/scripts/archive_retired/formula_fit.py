#!/usr/bin/env python3
# formula 档权重离线拟合 + 消融 + 离线重放验收（数据判决：七关系系数是否有效）。
# 输入：--formula-dump 产物（每候选逐特征）+ m12_cal 日志（LLM 选中标签）。
# 方法：题级 GroupKFold 坐标下降，权重保持公式形态（8 维线性，可读）；
#       消融 = 关一个特征（权重置 0）看重放覆盖率 Δ；判决 = 拟合权重≈0/变号 → 该
#       关系在此 benchmark 无贡献（如实报）。
# 用法: python bench/scripts/formula_fit.py \
#         --dump bench/runs/svec_eng/formula_dump.ndjson \
#         --log bench/runs/mechanisms/m12_cal.log
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "bench" / "scripts"))
import cheap_rerank_train as crt  # noqa: E402  load_labels

FEATS = ["bm25", "involves", "related", "next", "cooccur", "time", "fusion", "opposite"]
PRIOR = [0.9, 0.7, 0.9, 0.5, 0.6, 0.6, 0.4, -0.5]
GRID = [-1.0, -0.5, -0.2, 0.0, 0.1, 0.2, 0.3, 0.5, 0.7, 0.9, 1.2, 1.6, 2.0]


def load_dump(path):
    """→ {qid: {'dia': [...], 'X': [[8]...], 'gold': [...]}}（gold 从 llm.ndjson 补）。"""
    gold = {}
    for line in open(ROOT / "bench/runs/svec_eng/llm.ndjson", encoding="utf-8"):
        r = json.loads(line)
        if r.get("gold_evidence"):
            gold[r["question_id"]] = r["gold_evidence"]
    pools = {}
    for line in open(path, encoding="utf-8"):
        r = json.loads(line)
        p = pools.setdefault(r["qid"], {"dia": [], "X": [], "gold": gold.get(r["qid"], [])})
        p["dia"].append(r["dia"])
        p["X"].append([r[f] for f in FEATS])
    return pools


def topk_cov(pools, w, k=10):
    """重放：按 w 合成总分 → 每题 top-k dia → 覆盖率/any-hit（同 llm_rerank_compare 口径）。"""
    covs, ah, n = [], 0, 0
    for qid, p in pools.items():
        if not p["gold"]:
            continue
        n += 1
        scores = [sum(wi * xi for wi, xi in zip(w, x)) for x in p["X"]]
        top = sorted(range(len(scores)), key=lambda i: -scores[i])[:k]
        ret = {p["dia"][i] for i in top}
        c = len(set(p["gold"]) & ret) / len(p["gold"])
        covs.append(c)
        ah += bool(c)
    return sum(covs) / n, ah, n


def auc(p, w):
    scores = [sum(wi * xi for wi, xi in zip(w, x)) for x in p["X"]]
    pos = [s for s, d, x in zip(scores, p["dia"], p["X"]) if d in p["gold"]]
    neg = [s for s, d in zip(scores, p["dia"]) if d not in p["gold"]]
    if not pos or not neg:
        return None
    wins = 0.0
    for ps in pos:
        for ns in neg:
            wins += 1.0 if ps > ns else (0.5 if ps == ns else 0.0)
    return wins / (len(pos) * len(neg))


def mean_auc(pools, w, qids):
    vals = [auc(pools[q], w) for q in qids]
    vals = [v for v in vals if v is not None]
    return sum(vals) / len(vals) if vals else 0.0


def coordinate_descent(pools, train_qids, start, passes=4):
    w = list(start)
    best = mean_auc(pools, w, train_qids)
    for _ in range(passes):
        improved = False
        for j in range(8):
            for g in GRID:
                if abs(g - w[j]) < 1e-9:
                    continue
                w2 = list(w)
                w2[j] = g
                v = mean_auc(pools, w2, train_qids)
                if v > best + 1e-6:
                    w, best = w2, v
                    improved = True
        if not improved:
            break
    return w, best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default="bench/runs/svec_eng/formula_dump.ndjson")
    ap.add_argument("--log", default="bench/runs/mechanisms/m12_cal.log")
    a = ap.parse_args()

    pools = load_dump(a.dump)
    labels = crt.load_labels([a.log])
    # 拟合监督只用"被 LLM 看见过"的题（标签覆盖），覆盖率验收仍全 48 题
    fit_qids = sorted(q for q in pools if q in labels)
    print(f"[fit] 题数 {len(pools)}（有标签 {len(fit_qids)}）")

    # 先验版重放（与引擎端到端一致性校验）
    c0, ah0, n0 = topk_cov(pools, PRIOR)
    print(f"[replay] 先验权重 {PRIOR} → 覆盖率 {c0:.3f} any-hit {ah0}/{n0}")

    # GroupKFold（5-fold，题级）坐标下降：每折独立拟合 → held-out AUC（过拟合检查）
    folds = [set(fit_qids[i::5]) for i in range(5)]
    tr_aucs, cv_aucs, fold_ws = [], [], []
    for fi, te in enumerate(folds):
        tr = [q for q in fit_qids if q not in te]
        w, tr_auc = coordinate_descent(pools, tr, PRIOR)
        fold_ws.append(w)
        tr_aucs.append(tr_auc)
        cv_aucs.append(mean_auc(pools, w, sorted(te)))
        print(f"[fold{fi}] train_auc={tr_auc:.3f} heldout_auc={cv_aucs[-1]:.3f} w={['%.2f' % x for x in w]}")
    print(f"[cv] train {sum(tr_aucs)/5:.3f} / held-out {sum(cv_aucs)/5:.3f}"
          f"（差距 {sum(tr_aucs)/5 - sum(cv_aucs)/5:+.3f} = 过拟合面）")

    # 全量拟合（交付权重）：全部有标签题上拟合
    w_full, full_auc = coordinate_descent(pools, fit_qids, PRIOR)
    c1, ah1, _ = topk_cov(pools, w_full)
    print(f"[fit-all] train_auc={full_auc:.3f} 拟合权重 w={['%.3f' % x for x in w_full]}")
    print(f"[replay] 拟合权重 → 覆盖率 {c1:.3f} any-hit {ah1}/{n0}（先验 {c0:.3f}）")

    # 七关系消融（拟合权重上，关一个特征看重放 Δ）
    print("[abl] 特征消融（拟合权重，置 0 一个特征）：")
    for j, name in enumerate(FEATS):
        w2 = list(w_full)
        w2[j] = 0.0
        c2, _, _ = topk_cov(pools, w2)
        print(f"  -{name:<9} 覆盖率 {c2:.3f}（Δ {c2 - c1:+.3f}）")

    # 拟合权重 vs 先验的差距解读数据
    print("[gap] 拟合-先验 权重差：" + ", ".join(f"{n}:{f - p:+.2f}" for n, f, p in zip(FEATS, w_full, PRIOR)))
    out = {"prior": PRIOR, "fitted": w_full, "cv_heldout_auc": sum(cv_aucs) / 5,
           "fold_ws": fold_ws,
           "replay": {"prior_cov": c0, "fitted_cov": c1, "fitted_any_hit": ah1, "n": n0}}
    with open(ROOT / "bench/runs/svec_eng/formula_fit.json", "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    print("[out] bench/runs/svec_eng/formula_fit.json")


if __name__ == "__main__":
    main()
