# AME 词库（Lexicon Assets）

联想记忆引擎的语义知识库：**同义归并（syn_strong）/ 近义关联（syn_near）/ 反义对立（antonyms）
三分流**，经 M3 关键词管理器灌库使用（别名归一表 + RELATED 边）。

## 词条格式（schema）

```json
{"version": "...", "entries": [
  {"word": "失眠",
   "syn_strong": ["辗转反侧", "目不交睫"],          // 真同义/词形 → 别名归一表
   "syn_near": [{"word": "入睡困难", "score": 0.7}], // 近义（score>0.6、≤8 条）→ REL_SIMILAR 边
   "antonyms": ["安眠", "入睡"]}                     // 反义 → REL_OPPOSITE 边（系数 0.5 最低档）
]}
```

- **canonical 规则**：同义组以组内首词为标准词（词林/WordNet synset/箭头对锚词），
  成员统一归并到标准词，避免两两互指归一链/环。
- **内置标准词保护**：`失眠/压力` 等内置标准词不可被外部词典改写（防链式改写事故）。
- 旧版字段兼容：`synonyms`→syn_strong、`related`→syn_near。

## 资产总表

| 文件 | 词条 | strong 对 | near 对 | antonym 对 | 来源 |
|---|---|---|---|---|---|
| `lexicon_hf.json`（75MB） | 687,435 | — | — | — | 全库 804,609 合并词频 top 词 LLM 蒸馏（DeepSeek 批量，~10,500 次调用） |
| `lexicon_wordnet.json`（33MB） | 206,670 | 307,296 | 367,366 | 20,695 | WordNet 3.1（npm wordnet-db 包离线转换） |
| `lexicon_zh.json`（7.8MB） | 77,305 | 78,707 | 39,593 | 21,236 | funNLP 同义词词林（9,995 同义组/3,445 近义组）+ 反义词库（18,305 对） |
| `lexicon_zh2.json`（1.8MB） | 20,305 | 31,302 | 0 | 0 | funNLP `input/同义词.txt`（GBK 箭头对） |
| `lexicon.json` | 2,910 | — | — | — | LoCoMo 语料词表 LLM 蒸馏（默认管线） |
| `lexicon_gap.json` | 770 | — | — | — | WordNet 未覆盖高频词 LLM 补蒸馏 |
| `lexicon_en.json` | 12,127 | 20,173 | 33,448 | 13,128 | bench 英文语料（locomo10×20 加权 + longmemeval_s）top 12,127 LLM 蒸馏（DeepSeek，383 次调用） |
| `lexicon_src/` | — | — | — | — | 整理中间层：domain/（THUOCL 10 领域+IT+职业 163,181 词）、segmentation/merged.txt（9 个分词词典合并 678,211 词）、entities.txt（65,927 条 word\ttype）、manifest.json（逐文件解析报告） |

## 来源与许可

- **同义词词林/反义词库/清华开放中文词库/分词词典**：funNLP 索引仓库整理版
  （`dict/funNLP-master`，`bench/scripts/organize_dicts.py` 编码/格式整理）。
- **WordNet 3.1**：Princeton University 许可（见 wordnet-db 包内 LICENSE），
  `bench/scripts/convert_wordnet.py` 转换。
- **LLM 蒸馏产物**：本项目自产（DeepSeek API 批量生成），可自由使用。

## 加载方式

```bash
# 多文件拼接（并集合并，幂等；同词条同义/反义/关联取并集）
./build/bench/ame_bench --dataset locomo --data <数据文件> \
  --load-lexicon lexicon/lexicon.json,lexicon/lexicon_zh.json,lexicon/lexicon_zh2.json,lexicon/lexicon_hf.json
```

## 蒸馏工具复跑（增量断点续跑）

```bash
# 词频清单生成（合并 segmentation+domain 词频，过滤已覆盖）
python3 bench/scripts/prep_hf_vocab.py 5000
# 增量蒸馏（跳过 lexicon_hf.json 已蒸馏词，每批原子落盘，中断重跑即可续）
./build/bench/ame_bench --build-lexicon bench/data/vocab_hf.json \
  --lexicon-out lexicon/lexicon_hf.json --lexicon-topn 800000 \
  --concurrency 16 --cache-dir bench/cache
# 外部源转换（离线零 LLM）
python3 bench/scripts/convert_wordnet.py   # WordNet → lexicon_wordnet.json
python3 bench/scripts/convert_zh.py        # 词林+反义 → lexicon_zh.json
python3 bench/scripts/organize_dicts.py    # funNLP 源目录 → lexicon_src/
```

## 人名/专名归一表（names_zh.json，第六轮结论的专名域落地）

六轮词库调查的最终结论（docs/reports/lexicon_activation.md §9–§14）：等价归一表的正确域是
**专有名词**（人名/昵称/地名/机构名的同一性归并，严等价+守卫必要），普通词同义属于
graded 边表（--word-edges）。`names_zh.json` 即专名归一域的数据源：

```json
{"source": "names_zh",
 "groups": [{"canonical": "李建国", "aliases": ["小李", "李经理", "建国"]}]}
```

- **语义**：组内是人工确认的同一自然人断言（严等价）；同名同姓的两个不同人不得进同组。
- **加载**：`--name-alias lexicon/names_zh.json`（默认关；须在摄入前加载，与 --load-lexicon
  的摄入后灌库不同）。一处数据三通道贯通：M3 别名表（add_alias，守卫原样）+
  M1 提取词面注册（非称呼变体可从原文提取）+ M2 实体建档（canonical + 全变体 Alias 节点，
  称呼模式"老X/小X"提取的昵称摄入时经 Alias 命中归并）。
- **验收**：docs/reports/name_alias.md（中文小夹具 names_zh_mini.json，OFF 0/6 → ON 6/6，
  sample50 零回归）。
