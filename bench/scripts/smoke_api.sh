#!/bin/bash
cd /mnt/d/agent/memory/ame
python3 - <<'PYEOF'
import json, urllib.request
cfg = json.load(open("bench/config/deepseek.local.json"))
req = urllib.request.Request(
    cfg["base_url"] + "/chat/completions",
    data=json.dumps({"model": "deepseek-v4-flash",
                     "messages": [{"role": "user", "content": "只回答 ok"}],
                     "max_tokens": 512}).encode(),
    headers={"Authorization": "Bearer " + cfg["api_key"],
             "Content-Type": "application/json"})
with urllib.request.urlopen(req, timeout=60) as r:
    body = json.load(r)
    msg = body["choices"][0]["message"]
    print("content:", repr(msg.get("content")), "finish:", body["choices"][0]["finish_reason"],
          "usage:", body["usage"]["completion_tokens"])
PYEOF
