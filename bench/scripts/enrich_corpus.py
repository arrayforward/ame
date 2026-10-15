#!/usr/bin/env python3
# 写时语义增强（write-time enrichment）spike——离线标签管线。
# 读 locomo10 每个对话轮次 → 每轮一次 LLM 调用生成 ≤5 个检索标签
# （同义/改述/上下位）→ 逐轮 hash 磁盘缓存 → 产出增强语料 JSON（原轮次+tags 字段）。
# 设计要点：
#   - 写时一次性付 LLM 成本（离线/低频），推理侧零 LLM——标签靠词面挂载进
#     事件文本（loader 侧 " [tags: ...]"），词法通道与终排 BM25 自然命中；
#   - 泄漏禁令：prompt 明确禁止标签复读原句已出现的词（质检脚本统计违规率）；
#   - API 健康先探测，故障即停并报已消耗成本；
#   - 成本账：逐轮记录 prompt/completion tokens，汇总 tokens/轮 × 总轮数。
# 用法：python bench/scripts/enrich_corpus.py [--in bench/data/locomo10.json]
#         [--out bench/data/locomo10_enriched.json] [--sample 20] [--limit N]
import argparse
import concurrent.futures as cf
import hashlib
import json
import random
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_CFG = ROOT / "bench/config/deepseek.local.json"
CACHE = ROOT / "bench/cache_enrich"  # 逐轮 hash 缓存（gitignored，见 .gitignore）

PROMPT_TMPL = """You annotate a dialogue turn with retrieval tags for a long-term memory search engine.

Turn: "{text}"

Generate up to 5 retrieval tags for the turn: synonyms, paraphrases, hypernyms or hyponyms of its key content words/concepts (example: turn "...went to a party..." → tags: "gathering, celebration, social event, get-together").

Rules:
- Tags are single words or short phrases (at most 3 words).
- Each tag must capture a concept actually present in the turn, in wording DIFFERENT from the turn — someone searching later with different words should hit the tag.
- NEVER output a word or phrase that appears in the turn itself (no copying, no inflections of turn words, no names repeated).
- No more than 5 tags; fewer is fine. If the turn is pure small talk with nothing searchable, output NONE.
- Output only the comma-separated tags. No explanation, no numbering, no quotes."""


def load_cfg(path):
    j = json.loads(Path(path).read_text(encoding="utf-8"))
    key = j.get("api_key") or ""
    base = (j.get("base_url") or "https://api.deepseek.com").rstrip("/")
    if not key:
        sys.exit("缺少 api_key（bench/config/deepseek.local.json 或 --config）")
    return base, key, j.get("model", "deepseek-v4-flash")


def chat(base, key, model, prompt, max_retries=5):
    """单次 chat 调用；429/5xx 指数退避（与 C++ 客户端同策略）；返回 (text, usage)。"""
    body = json.dumps({
        "model": model, "messages": [{"role": "user", "content": prompt}],
        "temperature": 0, "max_tokens": 256,
        "thinking": {"type": "disabled"},  # deepseek-v4 推理模型：关思考（与 C++ 客户端同）
    }).encode("utf-8")
    delay = 2.0
    for attempt in range(max_retries):
        req = urllib.request.Request(base + "/chat/completions", data=body, headers={
            "Content-Type": "application/json", "Authorization": "Bearer " + key})
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                j = json.loads(r.read().decode("utf-8"))
            msg = j["choices"][0]["message"]["content"] or ""
            return msg.strip(), j.get("usage", {})
        except urllib.error.HTTPError as e:
            if e.code in (429, 500, 502, 503) and attempt < max_retries - 1:
                time.sleep(delay)
                delay *= 2
                continue
            raise RuntimeError(f"HTTP {e.code}: {e.read()[:200]!r}") from e
        except (urllib.error.URLError, TimeoutError) as e:
            if attempt < max_retries - 1:
                time.sleep(delay)
                delay *= 2
                continue
            raise RuntimeError(f"网络错误: {e}") from e
    raise RuntimeError("重试耗尽")


def parse_tags(resp):
    """模型输出 → 标签列表；NONE/空 → []。"""
    if not resp or resp.strip().upper().startswith("NONE"):
        return []
    out = []
    for tok in resp.replace("\n", ",").split(","):
        t = tok.strip().strip('"').strip(".")
        # 去掉编号前缀（"1. gathering" 之类）
        t = re.sub(r"^\d+[\.\)]\s*", "", t).strip()
        if t and len(t) <= 40:
            out.append(t)
    return out[:5]


def turn_key(sample_id, dia_id, text):
    return hashlib.sha256(f"{sample_id}|{dia_id}|{text}".encode("utf-8")).hexdigest()[:24]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", default=str(ROOT / "bench/data/locomo10.json"))
    ap.add_argument("--out", dest="out", default=str(ROOT / "bench/data/locomo10_enriched.json"))
    ap.add_argument("--config", default=str(DEFAULT_CFG))
    ap.add_argument("--model", default=None, help="覆盖 config 模型名")
    ap.add_argument("--concurrency", type=int, default=8)
    ap.add_argument("--sample", type=int, default=20, help="质检打印轮数（0=跳过）")
    ap.add_argument("--limit", type=int, default=0, help="只处理前 N 轮（调试用）")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    base, key, model = load_cfg(args.config)
    if args.model:
        model = args.model
    CACHE.mkdir(parents=True, exist_ok=True)

    # ---- API 健康探测：故障即停（不开始烧钱）----
    try:
        t0 = time.time()
        probe, _ = chat(base, key, model, 'Reply with exactly: ok')
        print(f"[probe] API 健康: {probe[:20]!r} ({time.time()-t0:.1f}s)")
    except RuntimeError as e:
        sys.exit(f"[probe] API 探测失败，停止：{e}")

    data = json.loads(Path(args.inp).read_text(encoding="utf-8"))
    # 拍平轮次清单：(定位路径, 文本)；定位 = (样本下标, session 名, 轮下标)
    flat = []
    for ci, c in enumerate(data):
        conv = c.get("conversation", {})
        si = 1
        while f"session_{si}" in conv:
            key_s = f"session_{si}"
            for ti, turn in enumerate(conv.get(key_s) or []):
                text = turn.get("text", "")
                if text:
                    flat.append((ci, key_s, ti, c.get("sample_id", str(ci)),
                                 turn.get("dia_id", ""), text))
            si += 1
    if args.limit:
        flat = flat[:args.limit]
    total = len(flat)
    print(f"[load] {args.inp}: {total} 轮待标注")

    stats = {"prompt": 0, "completion": 0, "calls": 0, "cache_hits": 0, "failed": 0}

    def work(item):
        ci, key_s, ti, sid, dia, text = item
        ck = turn_key(sid, dia, text)
        cf_ = CACHE / f"{ck}.json"
        if cf_.exists():
            stats["cache_hits"] += 1
            return ci, key_s, ti, json.loads(cf_.read_text(encoding="utf-8"))
        resp, usage = chat(base, key, model, PROMPT_TMPL.format(text=text))
        tags = parse_tags(resp)
        rec = {"tags": tags, "prompt_tokens": usage.get("prompt_tokens", 0),
               "completion_tokens": usage.get("completion_tokens", 0)}
        cf_.write_text(json.dumps(rec, ensure_ascii=False), encoding="utf-8")
        stats["prompt"] += rec["prompt_tokens"]
        stats["completion"] += rec["completion_tokens"]
        stats["calls"] += 1
        return ci, key_s, ti, rec

    results = {}
    done = 0
    with cf.ThreadPoolExecutor(max_workers=args.concurrency) as ex:
        futs = [ex.submit(work, it) for it in flat]
        for f in cf.as_completed(futs):
            try:
                ci, key_s, ti, rec = f.result()
                results[(ci, key_s, ti)] = rec["tags"]
            except RuntimeError as e:
                stats["failed"] += 1
                print(f"[warn] 单轮标注失败（跳过，不重跑）: {e}", file=sys.stderr)
            done += 1
            if done % 50 == 0 or done == total:
                print(f"[prog] {done}/{total} (calls={stats['calls']} "
                      f"cache={stats['cache_hits']} fail={stats['failed']})")

    # ---- 写回增强语料（原结构 + 每轮 tags 字段）----
    for ci, key_s, ti, *_ in flat:
        tags = results.get((ci, key_s, ti))
        if tags is not None:
            data[ci]["conversation"][key_s][ti]["tags"] = tags
    Path(args.out).write_text(json.dumps(data, ensure_ascii=False, indent=1),
                              encoding="utf-8")

    tagged = sum(1 for v in results.values() if v)
    # 泄漏自动质检：标签含原轮内容词（ASCII 小写 len≥3 切词）的违规率
    viol, tag_n = 0, 0
    for ci, key_s, ti, sid, dia, text in flat:
        tags = results.get((ci, key_s, ti)) or []
        words = set(re.findall(r"[a-z0-9]{3,}", text.lower()))
        for tg in tags:
            tag_n += 1
            if any(w in words for w in re.findall(r"[a-z0-9]{3,}", tg.lower())):
                viol += 1
    print(f"\n[done] 增强语料 → {args.out}")
    print(f"[cost] API 调用 {stats['calls']} 次（缓存命中 {stats['cache_hits']}，失败 {stats['failed']}）"
          f" | prompt {stats['prompt']} tok + completion {stats['completion']} tok"
          f" | 合计 {(stats['prompt']+stats['completion'])/max(1,stats['calls']):.0f} tok/轮")
    print(f"[qc] 有标签轮 {tagged}/{len(results)} | 标签 {tag_n} 个 | "
          f"泄漏违规（标签含原句词）{viol}/{tag_n} = {viol/max(1,tag_n):.1%}")

    # ---- 人工可读质检样例 ----
    if args.sample and results:
        rnd = random.Random(args.seed)
        picked = rnd.sample(flat, min(args.sample, len(flat)))
        print(f"\n[质检样例] 随机 {len(picked)} 轮（原文 + 标签）:")
        for ci, key_s, ti, sid, dia, text in picked:
            tags = results.get((ci, key_s, ti)) or []
            print(f"  [{dia}] {text[:110]}")
            print(f"      tags: {', '.join(tags) if tags else '(NONE)'}")


if __name__ == "__main__":
    main()
