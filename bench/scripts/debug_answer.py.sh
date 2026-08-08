#!/bin/bash
# 调试：直接验证答题 prompt 行为（证据可读日期版）
cd /mnt/d/agent/memory/ame
python3 - <<'PYEOF'
import json, urllib.request
cfg = json.load(open("bench/config/deepseek.local.json"))

ev = ["[2023-01-10] I moved to Seattle last week for my new job at Contoso.",
      "[2023-08-20] I moved again, now I live in Portland and work remotely.",
      "[2023-03-15] I started training for the Seattle marathon every morning."]
q = "Where did Alice live before moving to Portland?"

user = "相关记忆证据：\n"
for i, t in enumerate(ev):
    user += f"{i+1}. {t}\n"
user += f"问题: {q}\n简短回答；证据不足时只回答 NO INFORMATION。"

def call(thinking_disabled):
    body = {"model": "deepseek-v4-flash",
            "messages": [{"role": "system", "content":
                "你是个人记忆问答助手。只根据给出的证据回答问题，答案简短（几个词到一句话）。"
                "如果证据不足以回答，只输出 NO INFORMATION。"},
             {"role": "user", "content": user}],
            "temperature": 0.0, "max_tokens": 2048}
    if thinking_disabled:
        body["thinking"] = {"type": "disabled"}
    req = urllib.request.Request(cfg["base_url"] + "/chat/completions",
        data=json.dumps(body).encode(),
        headers={"Authorization": "Bearer " + cfg["api_key"],
                 "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r:
        resp = json.load(r)
    ch = resp["choices"][0]
    print(("no-think" if thinking_disabled else "think   "),
          "finish=", ch["finish_reason"], "content=", repr(ch["message"].get("content")))

call(True)
call(False)
PYEOF
