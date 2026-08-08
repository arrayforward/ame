#!/bin/bash
# 验证：① thinking disabled 参数 ② max_tokens>8192 上限
cd /mnt/d/agent/memory/ame
python3 - <<'PYEOF'
import json, urllib.request, time
cfg = json.load(open("bench/config/deepseek.local.json"))

def call(extra, mt=512):
    body = {"model": "deepseek-v4-flash",
            "messages": [{"role": "user", "content": "只回答 ok"}],
            "max_tokens": mt}
    body.update(extra)
    req = urllib.request.Request(cfg["base_url"] + "/chat/completions",
        data=json.dumps(body).encode(),
        headers={"Authorization": "Bearer " + cfg["api_key"],
                 "Content-Type": "application/json"})
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            resp = json.load(r)
        ch = resp["choices"][0]
        print(f"  OK {time.time()-t0:.1f}s finish={ch['finish_reason']} "
              f"content={msg_content(ch)!r} completion={resp['usage']['completion_tokens']}")
    except urllib.error.HTTPError as e:
        print("  HTTP", e.code, e.read().decode()[:200])

def msg_content(ch):
    m = ch["message"]
    return (m.get("content") or "")[:30] + ("|reasoning:" + str(len(m.get("reasoning_content") or "")))

print("A) thinking disabled:")
call({"thinking": {"type": "disabled"}})
print("B) chat_template_kwargs thinking=false:")
call({"chat_template_kwargs": {"thinking": False}})
print("C) enable_thinking=false:")
call({"enable_thinking": False})
print("D) max_tokens=16384 (不带其他参数):")
call({}, 16384)
PYEOF
