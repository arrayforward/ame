#!/usr/bin/env python3
# funNLP 杂乱词库整理（离线零 LLM）：编码统一(UTF-8 LF) + 格式解析 + 分类输出 + 去重
# 输入:  D:/agent/memory/dict/funNLP-master/funNLP-master/data
# 输出:  bench/data/lexicon_src/{synonyms,domain,segmentation,entities.txt,manifest.json}
# 幂等：输出目录先清后写。解析不了的行丢弃并计数（见 manifest 与控制台汇总表）。
import json, os, re, shutil, sys, collections

SRC = "/mnt/d/agent/memory/dict/funNLP-master/funNLP-master/data"
OUT = "/mnt/d/agent/memory/ame/bench/data/lexicon_src"

# ---------------- 编码与行尾 ----------------
def decode(path):
    """逐文件编码检测：utf-8-sig → utf-8 → gbk → gbk(容错)，返回 (text, encoding)"""
    b = open(path, "rb").read()
    for enc in ("utf-8-sig", "utf-8", "gbk"):
        try:
            return b.decode(enc), enc
        except UnicodeDecodeError:
            continue
    return b.decode("gbk", errors="replace"), "gbk-replace"  # GBK 系容错保底

def lines_of(text):
    """统一行尾：CRLF/CR/LF → 逻辑行列表"""
    return [ln for ln in re.split(r"\r\n|\r|\n", text) if ln.strip()]

manifest = []
def report(src_path, category, enc, total, parsed, failed, note):
    manifest.append({"src": src_path, "category": category, "encoding": enc,
                     "lines": total, "parsed": parsed, "failed": failed, "note": note})

# ---------------- 通用解析 ----------------
POS_PREFIX = re.compile(r"^[a-z]{1,3}(?=[\u4e00-\u9fff])")  # 词性前缀，如 "n丈"

def parse_word_freq(line):
    """解析 (word, freq?)：tab/空格分列；剥离词性前缀；词本身含字母数字前缀的丢弃"""
    parts = re.split(r"\t+| +", line.strip())
    parts = [p for p in parts if p]
    if not parts:
        return None
    w = POS_PREFIX.sub("", parts[0])
    if not w or not re.search(r"[\u4e00-\u9fffA-Za-z]", w):
        return None
    freq = None
    for p in parts[1:]:
        if p.isdigit():
            freq = int(p)
            break
    return (w, freq)

# ---------------- 1. THUOCL 领域词库 + professions ----------------
def do_domain():
    cats = [d for d in os.listdir(SRC)
            if d.startswith("THUOCL_") and os.path.isdir(os.path.join(SRC, d))]
    os.makedirs(f"{OUT}/domain", exist_ok=True)
    total = {}
    for d in sorted(cats):
        cat = d[len("THUOCL_"):]
        path = os.path.join(SRC, d, d + ".txt")
        if not os.path.exists(path):
            continue
        text, enc = decode(path)
        rows, failed = [], 0
        for ln in lines_of(text):
            r = parse_word_freq(ln)
            if r:
                rows.append((r[0], r[1] if r[1] is not None else 0))
            else:
                failed += 1
        # 领域内去重（保留最高频次）
        best = {}
        for w, f in rows:
            if w not in best or f > best[w]:
                best[w] = f
        with open(f"{OUT}/domain/{cat}.txt", "w", encoding="utf-8", newline="\n") as f:
            for w, fr in sorted(best.items(), key=lambda x: -x[1]):
                f.write(f"{w}\t{fr}\t{cat}\n")
        total[cat] = len(best)
        report(f"THUOCL_{d[7:]}", "domain", enc, len(rows) + failed, len(best), failed,
               "词\\t频次，部分行无频次按 0")
    # IT 目录无 THUOCL_ 前缀（目录名 IT，文件名 THUOCL_it.txt）
    it_path = os.path.join(SRC, "IT/THUOCL_it.txt")
    if os.path.exists(it_path):
        text, enc = decode(it_path)
        rows = [parse_word_freq(ln) for ln in lines_of(text)]
        best = {}
        for r in rows:
            if r and (r[0] not in best or (r[1] or 0) > best[r[0]]):
                best[r[0]] = r[1] or 0
        with open(f"{OUT}/domain/it.txt", "w", encoding="utf-8", newline="\n") as f:
            for w, fr in sorted(best.items(), key=lambda x: -x[1]):
                f.write(f"{w}\t{fr}\tit\n")
        total["it"] = len(best)
        report("IT/THUOCL_it.txt", "domain", enc, len(rows), len(best),
               sum(1 for r in rows if not r), "目录名无 THUOCL_ 前缀")
    # professions
    p = os.path.join(SRC, "professions/professions.txt")
    if os.path.exists(p):
        text, enc = decode(p)
        words = [ln.strip() for ln in lines_of(text) if ln.strip()]
        with open(f"{OUT}/domain/professions.txt", "w", encoding="utf-8", newline="\n") as f:
            for w in sorted(set(words)):
                f.write(f"{w}\t0\tprofession\n")
        total["professions"] = len(set(words))
        report("professions/professions.txt", "domain", enc, len(words), len(set(words)),
               0, "一行一词")
    return total

# ---------------- 2. 同义词.txt (GBK) → 词林风格分组 ----------------
def do_synonyms():
    path = os.path.join(SRC, "input/同义词.txt")
    if not os.path.exists(path):
        report("input/同义词.txt", "synonyms", "-", 0, 0, 0, "文件缺失")
        return 0
    text, enc = decode(path)
    os.makedirs(f"{OUT}/synonyms", exist_ok=True)
    groups, failed = [], 0
    for ln in lines_of(text):
        ln = ln.strip()
        if not ln:
            continue
        # 箭头对格式 "word→syn"（词林风格归并：word 为标准词）
        if "→" in ln:
            a, b = ln.split("→", 1)
            a, b = a.strip(), b.strip()
            if a and b and re.search(r"[\u4e00-\u9fff]", a + b):
                groups.append(f"G= {a} {b}")
            else:
                failed += 1
        # 已是词林格式（编码+标记符）直接保留；否则按空白/逗号分组
        elif re.match(r"^[A-Za-z]{2}\d{2}[A-Z]\d{2}[=#@]", ln):
            groups.append(ln)
        else:
            ws = [w for w in re.split(r"[\s,，、/]+", ln) if re.search(r"[\u4e00-\u9fff]", w)]
            if len(ws) >= 2:
                groups.append("G= " + " ".join(ws))  # 伪编码 + "=" 标记
            else:
                failed += 1
    with open(f"{OUT}/synonyms/groups.txt", "w", encoding="utf-8", newline="\n") as f:
        for g in groups:
            f.write(g + "\n")
    report("input/同义词.txt", "synonyms", enc, len(groups) + failed, len(groups), failed,
           "箭头对 word→syn（word 为标准词）；词林格式保留；其余按空白/逗号分组")
    return len(groups)

# ---------------- 3. input 分词词典合并 ----------------
SEG_FILES = ["30wdict_utf8.txt", "30wChinsesSeqDic_clean.txt", "dict.txt",
             "httpcws_dict.txt", "词.txt", "输入法.txt", "输入2.TXT", "out.txt",
             "fingerDic.txt"]
def do_segmentation():
    os.makedirs(f"{OUT}/segmentation", exist_ok=True)
    best = {}   # word -> (max_freq, [sources])
    for name in SEG_FILES:
        path = os.path.join(SRC, "input", name)
        if not os.path.exists(path):
            continue
        text, enc = decode(path)
        parsed = failed = 0
        for ln in lines_of(text):
            r = parse_word_freq(ln)
            if not r:
                failed += 1
                continue
            w, fr = r
            fr = fr or 0
            parsed += 1
            if w not in best:
                best[w] = [fr, [name]]
            else:
                if fr > best[w][0]:
                    best[w][0] = fr
                if name not in best[w][1]:
                    best[w][1].append(name)
        report(f"input/{name}", "segmentation", enc, parsed + failed, parsed, failed,
               "词性前缀剥离/频次可选，跨文件合并")
    with open(f"{OUT}/segmentation/merged.txt", "w", encoding="utf-8", newline="\n") as f:
        for w, (fr, srcs) in sorted(best.items(), key=lambda x: -x[1][0]):
            f.write(f"{w}\t{fr}\t{','.join(srcs)}\n")
    return len(best)

# ---------------- 4. 实体类型词典（地名/历史名人/职业）----------------
def do_entities():
    rows = []
    for cat, typ in (("diming", "place"), ("lishimingren", "person"),
                     ("professions", "occupation")):
        p = f"{OUT}/domain/{cat}.txt"
        if not os.path.exists(p):
            continue
        for ln in open(p, encoding="utf-8"):
            w = ln.split("\t")[0].strip()
            if w:
                rows.append((w, typ))
    seen = {}
    for w, t in rows:
        seen.setdefault(w, t)  # 首个类型优先
    with open(f"{OUT}/entities.txt", "w", encoding="utf-8", newline="\n") as f:
        for w, t in sorted(seen.items()):
            f.write(f"{w}\t{t}\n")
    return len(seen)

def main():
    if os.path.exists(OUT):
        shutil.rmtree(OUT)  # 幂等：先清后写
    os.makedirs(OUT)
    n_dom = do_domain()
    n_syn = do_synonyms()
    n_seg = do_segmentation()
    n_ent = do_entities()
    with open(f"{OUT}/manifest.json", "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1)
    print("== 整理汇总 ==")
    print(f"{'类别':<14}{'词条数':>10}{'解析失败':>10}")
    for m in manifest:
        print(f"{m['src']:<40} {m['parsed']:>8} {m['failed']:>8}  [{m['encoding']}]")
    print(f"domain 类别: {n_dom}")
    print(f"synonyms 组: {n_syn} / segmentation 合并词条: {n_seg} / entities: {n_ent}")

if __name__ == "__main__":
    main()
