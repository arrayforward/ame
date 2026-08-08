#!/usr/bin/env python3
# 用 jsDelivr data API 探查候选仓库的文件结构，找同义词词林真实路径
import json, urllib.request

REPOS = ["fighting41love/Cilin_similarity", "ysc/cilin", "liuhuanyong/SynonymsDictionary",
         "thunlp/OpenHowNet", "fighting41love/funNLP"]

def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "curl/8"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)

for repo in REPOS:
    try:
        meta = get(f"https://data.jsdelivr.com/v1/packages/gh/{repo}")
        ver = meta.get("version") or (meta.get("versions") or [{}])[0].get("version")
        if not ver:
            print(repo, "no version"); continue
        files = get(f"https://data.jsdelivr.com/v1/packages/gh/{repo}@{ver}?structure=flat")
        names = [f["name"] for f in files.get("files", [])]
        hits = [n for n in names if any(k in n.lower() for k in ("cilin", "词林", "synonym", "hownet", "同义"))]
        print(repo, "@", ver, "files:", len(names), "hits:", hits[:8])
    except Exception as e:
        print(repo, "ERR", str(e)[:80])
