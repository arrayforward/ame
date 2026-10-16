#!/usr/bin/env python3
# OpenAI 兼容双端 shim（商业对标基础设施，系统 Python 跑，零额外依赖）：
#   POST /v1/chat/completions  → 磁盘缓存(FNV/sha1 请求体) + 转发 DeepSeek（可复跑零成本）
#   POST /v1/embeddings        → 本地 bge-small-en-v1.5 int8 ONNX 直接推理（DeepSeek 无 embedding API 的本地替代）
# mem0/graphiti 等各家客户端把 openai_base_url 指向本 shim 即完成"LLM 缓存可复跑 + 本地嵌入"。
# 用法: python bench/scripts/openai_shim.py --port 8399 --cache bench/runs/commercial/llm_cache
import argparse
import hashlib
import json
import threading
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent.parent
import sys
sys.path.insert(0, str(ROOT / "bench" / "scripts"))
import svec_eval as se  # noqa: E402

STATS = {"chat_hit": 0, "chat_miss": 0, "chat_in_tok": 0, "chat_out_tok": 0, "emb": 0}


def estimate_tokens(body):
    try:
        j = json.loads(body)
    except Exception:
        return 0, 0
    inp = 0
    for m in j.get("messages", []):
        inp += len(str(m.get("content", ""))) // 4 + 8
    out = j.get("max_tokens", 0) or 0
    return inp, out


class Handler(BaseHTTPRequestHandler):
    ds_base = None
    ds_key = None
    cache_dir = None
    emb = None

    def log_message(self, *a):  # 静默
        pass

    def _send(self, code, obj):
        data = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode("utf-8", "replace")
        if self.path.rstrip("/").endswith("/chat/completions"):
            self._chat(body)
        elif self.path.rstrip("/").endswith("/embeddings"):
            self._embed(body)
        else:
            self._send(404, {"error": "unknown path"})

    def _chat(self, body):
        key = hashlib.sha1(body.encode()).hexdigest()
        f = self.cache_dir / f"{key}.json"
        if f.exists():
            STATS["chat_hit"] += 1
            self._send(200, json.load(open(f, encoding="utf-8")))
            return
        STATS["chat_miss"] += 1
        inp, out = estimate_tokens(body)
        STATS["chat_in_tok"] += inp
        STATS["chat_out_tok"] += out
        req = urllib.request.Request(
            self.ds_base + "/chat/completions", data=body.encode(),
            headers={"Authorization": "Bearer " + self.ds_key,
                     "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=600) as r:
                resp = json.loads(r.read().decode())
        except Exception as e:  # 网络/限流 → 透传错误，调用方各自重试
            self._send(502, {"error": str(e)})
            return
        tmp = f.with_suffix(".tmp")
        json.dump(resp, open(tmp, "w", encoding="utf-8"), ensure_ascii=False)
        tmp.replace(f)
        self._send(200, resp)

    def _embed(self, body):
        STATS["emb"] += 1
        j = json.loads(body)
        texts = [i["input"] if isinstance(i, dict) else i for i in j["input"]] \
            if isinstance(j["input"], list) else [j["input"]]
        vecs = self.emb.embed_batch(list(texts), 64)
        # OpenAI dimensions 参数兼容：客户端声明的维度高于本地模型时零填充
        # （余弦相似度对双侧等长零填充不变；mem0 默认声明 1536，本地 bge=384）。
        dim_req = j.get("dimensions")
        if dim_req and dim_req > vecs.shape[1]:
            pad = np.zeros((vecs.shape[0], dim_req), dtype=np.float32)
            pad[:, : vecs.shape[1]] = vecs
            vecs = pad
        self._send(200, {"object": "list",
                         "data": [{"object": "embedding", "index": i, "embedding": v.tolist()}
                                  for i, v in enumerate(vecs)]})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8399)
    ap.add_argument("--cache", default="bench/runs/commercial/llm_cache")
    ap.add_argument("--config", default="bench/config/deepseek.local.json")
    ap.add_argument("--model-dir", default="bench/models/bge-small-en-v1.5")
    a = ap.parse_args()

    cfg = json.load(open(ROOT / a.config, encoding="utf-8"))
    Handler.ds_base = cfg.get("base_url", "https://api.deepseek.com").rstrip("/")
    Handler.ds_key = cfg["api_key"]
    Handler.cache_dir = ROOT / a.cache
    Handler.cache_dir.mkdir(parents=True, exist_ok=True)
    Handler.emb = se.Embedder(str(ROOT / a.model_dir))
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"[shim] :{a.port} chat→{Handler.ds_base}(cache {Handler.cache_dir}) "
          f"emb→local bge", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print(json.dumps(STATS), flush=True)


if __name__ == "__main__":
    main()
