#!/bin/bash
# 单点调试：LoCoMo 第一个 session 走 LlmExtractor 同款 prompt，打印原始响应诊断信息
cd /mnt/d/agent/memory/ame
python3 - <<'PYEOF'
import json, urllib.request, time

cfg = json.load(open("bench/config/deepseek.local.json"))
data = json.load(open("bench/data/locomo10.json"))
conv = data[0]["conversation"]
sess = conv["session_1"]
print("session_1 turns:", len(sess))

# 与 LlmExtractor::extract_session 完全一致的 prompt 构造
user = ("对以下对话轮逐轮提取记忆要素。输出 JSON 数组，每轮一个对象：\n"
        "{\"type\":事件类型, \"entities\":[{\"surface\":名称,\"role\":主体|人物|客体,"
        "\"type\":person|object|org}], \"keywords\":[{\"word\":词,"
        "\"category\":action|attr|topic|sentiment|concept}]}。\n"
        "只输出 JSON 数组，不要解释。对话轮：\n")
for i, t in enumerate(sess):
    user += f"{i}. {t['speaker']}: {t['text']}\n"
print("prompt bytes:", len(user.encode()))

body = {"model": "deepseek-v4-flash",
        "messages": [{"role": "system", "content": "你是记忆要素提取器，严格输出 JSON。"},
                     {"role": "user", "content": user}],
        "temperature": 0.0, "max_tokens": 8192,
        "response_format": {"type": "json_object"}}
req = urllib.request.Request(cfg["base_url"] + "/chat/completions",
    data=json.dumps(body).encode(),
    headers={"Authorization": "Bearer " + cfg["api_key"],
             "Content-Type": "application/json"})
t0 = time.time()
try:
    with urllib.request.urlopen(req, timeout=180) as r:
        resp = json.load(r)
except urllib.error.HTTPError as e:
    print("HTTP", e.code, e.read().decode()[:500]); raise SystemExit
dt = time.time() - t0
ch = resp["choices"][0]
msg = ch["message"]
print(f"耗时 {dt:.1f}s finish_reason={ch.get('finish_reason')} "
      f"completion_tokens={resp['usage']['completion_tokens']}")
print("content len:", len(msg.get("content") or ""))
print("reasoning len:", len(msg.get("reasoning_content") or ""))
c = msg.get("content") or ""
print("content[:300]:", c[:300])
print("content[-200:]:", c[-200:])
PYEOF
