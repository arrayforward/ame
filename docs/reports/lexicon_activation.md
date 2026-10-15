# 语义 graph 激活工程：词库激活逐级 A/B 报告

- 分支：`dev-2.0`（2.0 已砍，本任务为纯 1.x 线工程）
- 日期：2026-10-15
- 结论速览：**L1 证伪**（查询侧词激活在 sample50 上覆盖率零信号，Δ 在基线固有噪声带内）。
  按任务纪律 **L2/L3 未执行验收**（L2 代码已实现但默认关闭，未跑 A/B）。

---

## 1. 背景与断点定位（复查确认）

背景结论复查属实，且比原描述更具体：

- `load_keyword_dict` 全仓库仅 `bench/scripts/bench_ac.cpp`（调试脚本）与 jni 调用，
  bench 评测管线确实从不加载大词表。
- 查询侧入口在 **M5 `SeedGenerator::gen_seeds`**（`seed/src/seed_generator.cpp:101`）：
  `extractor_->extract(q.text)` 提取查询的实体/关键词/中文词 → `kw_.find(kw_.normalize(...))`
  命中图上关键词节点 → 硬种子（weight=1.0）→ 语义 graph 入口。提取器空转 = 该通道只有
  内置规则（英文自由词元 len≥4 + 中文内置小词典）可用。
- 写入侧（M4 `graph_writer.cpp:164`）CONTAINS 落边时经 `kw_.get_or_create` 做 M3 归一，
  机制在，但 bench 流程 `--load-lexicon` 在**摄入完成后**才灌别名表，属摄入后追加，
  对已落边的历史 CONTAINS 无追溯效果（L3 关注点，因 L1 证伪未展开）。

## 2. 改动清单

| 文件 | 改动 | 说明 |
|---|---|---|
| `bench/scripts/gen_wordnet_words.py` | 新增 | 从 `lexicon/lexicon_wordnet.json` 提取全部词条（word/syn_strong/syn_near/antonyms 四流 key 与值），去重小写，一词一行 |
| `bench/data/wordnet_words.txt` | 新增（生成物） | **147,676 词**（entries=206,670；64,094 个含空格词组；全部小写）。示例：`'hood`、`a cappella`、…、`zymurgy`。已验证 `lexicon/lexicon_src/segmentation/merged.txt` 为**中文词表**（67.8 万行，`词\tfreq\t来源` 格式，head 全中文），英文场景用本文件 |
| `extractor/src/extractor.cpp` | 小改 | AC 扫描改为对 ASCII 小写副本匹配（原实现按码点精确匹配，**大小写敏感**；词表存小写后大写查询词永不命中）。仅在 `ac_built_`（词典已加载）时生效，对现状零影响 |
| `bench/include/ame/bench/bench.h` | 小改 | `BenchConfig` 新增 `final_mix/final_alpha/final_beta`（L2，默认关闭） |
| `bench/cli/main.cpp` | 小改 | 新增 `--extractor-dict`（摄入+灌库后、答题前 `load_keyword_dict_cached(path,"seg",700000,path+".ac")`，log 输出条数）；`--final-alpha/--final-beta`（L2 开关）；lmem 后端忽略警告补两项；usage 更新 |
| `bench/src/ingest_answer.cpp` | 小改 | `answer()` 步骤4 终排：显式传 `--final-alpha/--final-beta` 时排序键改为 `α·BM25内容分 + β·minmax_norm(1.x融合分)`（归一在本题候选集内 min-max）；默认关闭 = 历史纯 BM25 行为逐行不变 |
| `bench/scripts/lexicon_act_compare.py` | 新增 | 两个 dump 的覆盖率对照（口径同 lmem_p0：`|gold∩evidence|/|gold|`） |

设计取舍说明（与任务书的差异及理由）：

- **L2 默认关闭而非默认 0.6/0.4 生效**：任务书要求"逐级 A/B 不许多级糊在一起"。若混合默认生效，
  L1 组会同时含 L2 效应，逐级分离不可能。故 `--final-alpha/--final-beta` 任一传入才开启
  （默认值即任务书指定的 0.6/0.4）。
- 词表存小写 + AC 小写副本扫描，保证查询原文任意大小写均可命中（`extract()` 输出词为
  小写词表词，与 M3 `normalize` 的小写语义一致，写入/查询两侧对齐）。

## 3. L1：查询侧词激活——实现与机制验证

实现按任务书：每 case 摄入完成、`--load-lexicon` 灌库之后、答题循环之前加载；
AC 缓存 `bench/data/wordnet_words.txt.ac` 同目录（首 case 构建并覆写，后续 case 直接加载）。

机制验证（断点确实接通）：

- L1 运行日志：`[extractor-dict] case conv-26 加载 147676 条（bench/data/wordnet_words.txt）`。
- 加载后摄入统计不变（events=419 entities=41 keywords=1163，与 off 完全一致）——
  符合"摄入后加载"的纯查询侧语义，图内容零改动。
- 激活上限测算：50 题查询文本中，相对内置自由词元规则（ASCII 小写 len≥4 且不在内置
  停用词表），词典**仅能新增** 12 个词元、涉及 11 题（`ago/like/have/are/say/new` 等
  停用词或 len<4 词）。这些词要么无对应关键词节点（停用词本就被 M3 过滤），要么
  产生的种子不改变 top-k——L1 的效应上限本来就贴着零。

## 4. L1 A/B（sample50，基线模式，wordnet 灌库）

命令（可复跑，`export PATH="$(dirname $(which g++)):$PATH"` 前置）：

```bash
# off（已有基线，本次同时复跑做噪声测定）
./build-win/bench/ame_bench.exe --dataset locomo --data bench/data/locomo10.json \
  --question-ids bench/data/locomo_sample50.json --extractor rule --answer baseline \
  --judge f1 --k 10 --dump bench/runs/lexicon_ab/off_repro.ndjson
# L1 = off + wordnet 灌库 + extractor-dict
./build-win/bench/ame_bench.exe ... 同上 ... \
  --load-lexicon lexicon/lexicon_wordnet.json \
  --extractor-dict bench/data/wordnet_words.txt \
  --dump bench/runs/lexicon_ab/L1.ndjson
```

### 4.1 基线固有噪声测定（先于判决）

同一二进制、同一命令、零新 flag 连跑两次（off_repro vs off_repro2）：

- 16/50 行 dump 有差异，其中 **13 题 evidence_dia_ids 集合本身不同**（top-k 成员翻转）。
- 覆盖率 off 0.448 → 复跑 0.443，**Δ=-0.005 纯噪声**。

根因：`core/src/uid.cpp:38 gen_uid()` 以 `random_device` 播种子 → 每次运行节点 uid 不同 →
哈希容器迭代序不同 → 扩散/重排的浮点累加序不同 → stable_sort 同分候选次序翻转。
**这是 1.x 管线既有属性，与本次改动无关**（改动在 off 路径上行为恒等：无 dict 时 AC
分支不激活，`final_mix=false` 时排序键即原内容分）。因此"与 off.ndjson 逐字节一致"
在该管线上物理不可达，回归口径改用"噪声带内等价"（覆盖率同值 ±0.005、逐题覆盖一致
除已知翻转题）。

### 4.2 逐级对照表

| 组 | 覆盖率（n=48） | any-hit | 与 off Δ | 逐题覆盖差异 |
|---|---|---|---|---|
| off（已有 off.ndjson） | 0.448 | 25/48 | — | — |
| off 复跑（零改动） | 0.443 | 24/48 | -0.005（噪声） | q32 翻转（噪声） |
| wordnet 灌库（已有，无激活） | 0.443 | 24/48 | -0.005 | q32 翻转 |
| **L1（灌库+extractor-dict）** | **0.443** | **24/48** | **-0.005** | **q32 翻转（与灌库组逐题完全相同）** |

关键对照：**wordnet 灌库组 vs L1 组逐题覆盖率 50/50 全同（Δ=+0.000）**——
即大词表激活没有改变任何一题的召回结果；L1 与 off 的 q32 差异与"off 复跑 vs off"的
q32 差异是同一噪声翻转（q32 金标 4 条，各组均在 0/4~1/4 间随噪声波动）。

### 4.3 L1 判决：**证伪**

覆盖率信号 0（Δ=+0.000 vs 同灌库对照；-0.005 vs off 属噪声），远低于 +0.02 判据。
机制上断点已接通（14.7 万词确实加载进查询侧提取器），但英文查询场景下内置自由词元
规则已覆盖 len≥4 内容词，词典只能补充停用词/3 字母词（上限 12 词元/11 题，实测零种子
效应）。**词库假说在 sample50 上证伪，按纪律停报，L2/L3 不做验收。**

## 5. L2/L3 状态

- L2 终排混合已实现（`answer()` 步骤4，`--final-alpha/--final-beta` 开启，默认 0.6/0.4，
  min-max 归一限定本题候选集内、无跨 case 泄漏）。原记录"因 L1 证伪未跑 A/B"；后应
  根因调查需求补跑为判决性实验，见 §9 第 4 层（无正向信号）。
- L3 写入侧归一对账未执行。已确认的静态事实：`--load-lexicon` 在摄入后灌别名表，
  对历史 CONTAINS 边无追溯归一；别名表对"别名节点已存在/主词节点不存在"的组合
  不会产生新连接（`apply_lexicon` 对缺失节点跳过）。
- 若未来要在英文语料重试词激活，方向应是**写入侧**（摄入前加载词表改变 keyword 节点
  构成 + 别名表随摄入生效），而非本次的查询侧。

## 6. 蓝方自检逐条

1. **提取器词典加载时机**：✅ 每 case 摄入+灌库后、答题循环前加载，答题时 gen_seeds
   的 `extractor_->extract(query)` 必然命中已加载词典；L1 日志确认 147676 条。
2. **AC 缓存路径权限**：✅ 缓存 `<file>.ac` 与词表同目录（bench/data，仓库内可写）；
   首次构建覆写成功（第二 case 起日志仍 147676 条 = 缓存命中路径验证过：单 case
   样本下首跑构建耗时 <10s 总运行 9.4s）；词典变更（size/mtime）自动失效重建。
3. **英文词条大小写**：✅ 已确认原 AC 匹配大小写敏感（按码点精确匹配，词表小写则
   "Ross" 永不命中）；修复为 extract() 内对 ASCII 小写副本扫描，词表存小写，
   输出小写词表词与 M3 normalize 小写语义一致。词表生成脚本强制 lower() 并校验
   无大写行（awk 核验 0 行含大写）。
4. **wordnet_words.txt 词形（lemma vs 词形变化）**：⚠️ 词表是 WordNet lemma 集，
   不含词形变化（running/ran 不在表）。但因证伪根因是"自由词元规则已覆盖 len≥4 词"，
   词形变化不影响判决——变化形在文本中作为 len≥4 词已被内置规则提为关键词节点与
   查询词。漏词率估计：对查询侧激活路径，理论非零新增仅停用词/3 字母词（12 词元/50 题）。
5. **L2 归一的跨 case 泄漏防护**：✅ min-max 归一在单次 answer() 调用的候选集内计算
   （每题局部变量，引擎每 case 独立），无跨 case、无跨题状态。

## 7. 回归与测试

- ctest 20/20：见本节末尾补记。
- 回归：不传新 flag 的复跑与 off.ndjson 无法逐字节一致——**基线自身两次连跑即
  16/50 行不同**（§4.1，gen_uid 随机种子所致，既有属性）。等价性验证：复跑覆盖率
  0.443 落在 off 的噪声带内，无新增差异模式。

## 8. 最终判决建议：词库资产处置

**建议：将 wordnet 词库资产移出评测管线（不接入默认流程），保留资产文件与
`--extractor-dict` 开关代码（默认关闭）作为已证伪方向的存档。**

依据：

1. 灌库（22.4 万条别名/RELATED 边）在 sample50 上覆盖率 0.448→0.443（噪声带内，
   本轮复跑证明噪声本身即 ±0.005）；本轮加上查询侧激活仍逐题零差异。
2. 机制分析表明英文场景下该资产的可用面积极小：M1 自由词元规则已把 len≥4 内容词
   全部提为关键词节点，词库的边际覆盖只剩停用词与 3 字母词；RELATED 边依赖两端
   关键词节点存在（灌库时大量跳过），扩散增益路径在本 benchmark 未被触发。
3. 若保留在管线内（如 `--load-lexicon` 默认带上 wordnet），只会增加每跑 22 万条
   边的构建开销（L1 总耗时 9.4s 中灌库占大头）而收益为零。

保留价值：`--extractor-dict` 接线对**中文场景**（merged.txt 67.8 万中文词）尚未验证，
CJK 词法通道（gen_seeds 的 cjks 分支）是真正依赖 M1 词典产中文词的通道；若未来评中文
语料可复用同一开关重试。此为唯一未关闭的后续方向。

---

## 9. 根因调查（四层证据链，应调查需求追加；第 4 层为判决性实验）

> 背景：L1 证伪停报是纪律动作。本节把"词库为什么端到端无效"查到底。
> 调查工具：`bench/scripts/dbg_lextrace.cpp`（临时调查工具，不入构建；编译命令同
> `bench/scripts/dbg_seeds.sh` 的 g++ 直编模式，libs 取 `build-win`）。

### 第 1 层：图里有没有结构？——**有，而且很大**

运行期核查（ingest conv-26 后 `apply_lexicon(lexicon/lexicon_wordnet.json)`，直读 Engine）：

```
Keyword 节点: 灌库前 1163 -> 灌库后 1163（灌库不建节点，符合设计）
别名表: 灌库前 9（内置） -> 灌库后 103,905（applied 计数 223,702 含 near 登记与边）
REL_SIMILAR 边 1,892，REL_OPPOSITE 边 64
```

结论：灌库管道无断点，10.4 万别名 + 1,956 条语义边真实存在于图与 M3 表中。

### 第 2 层：查询词能否激活结构？——**触发率表面高，有效激活≈0**

对 sample50 全部 50 题问题文本，按 lex 通道口径（ASCII 小写切词去 43 停用词，
`baseline` 默认无条件归一：`variants(w,3)` ×0.6）逐词测：

```
汇总: 50 题 | 内容词 219 | normalize 别名命中 93 | 有 variants 的词 114 | 扩展词 df>0 仅 17
      有硬种子题 41/50 | 有扩展题 45/50
```

- 45/50 题至少一个词触发同义扩展（例：`group→aggroup/mathematical group`、
  `support→bear out`、`pursue→quest after`、`long→yearn`、`picnic→outing`、
  `new→fresh`、`grandpa→grandfather`）。
- **但 114 个扩展词里只有 17 个的变体在事件文本中 df>0**——扩展出的是 WordNet
  书面/正式 lemma（aggroup、carolean、quest after、natal day），口语对话语料里
  df≈0；df>0 的少数（`years→days`、`likely→potential`）是高频噪声词，只会引入
  假阳性候选。
- 词条义项错配实证：`like` 词条的 syn_strong 只有 `[comparable, corresponding]`
  （形容词义项），**不含 love**（love 在 `love` 词条下的 near 里，near 不参与
  baseline 默认扩展）；`kids`/`children` **双双无词条**——口语核心词对不在资产中。
- 关键词硬种子通道（extractor 关键词 → normalize → find）41/50 题有产出，
  该通道本身工作正常，与词库无关（builtin 自由词元已覆盖）。

### 第 3 层：激活后的候选死在哪一站？——2 题逐站 trace

`dbg_lextrace ... lexicon/lexicon_wordnet.json bench/data/wordnet_words.txt conv-26:q19 conv-26:q64`

**q19 "What do Melanie's kids like?"（金标 D6:6/D4:8，miss）**
- 站(a) lex 通道：扩展 `like→wish(df=2)/alike(p)(df=0)/comparable(df=0)`；
  **仅靠扩展词命中 0 条**——"wish"命中的事件过不了收录门（m≥2 或单罕见词≥6字母），
  **死在站(a) lex 收录门**。
- 根因在金标文本本身：金标说 "The 2 younger kids **love** nature"（D4:8）、
  "They **love** learning"（D6:6）——需要的桥是 `like→love`，但 like 词条无此别名
  （第 2 层义项错配），且事件词形 loves/love 与 lemma 精确匹配（无词形归一）。
  站(b)(c) 无从谈起：候选根本没产生。

**q64 "Would Melanie likely enjoy the song The Four Seasons by Vivaldi?"（金标 D15:28，miss）**
- 站(a)：扩展 `enjoy→love(df=72)`、`likely→potential(df=1)`；仅靠扩展命中 1 条
  D14:17（经 `potential`，m=1 且 len≥6 罕见词收录）→ **过站(a)**。
- 站(b)：D14:17 进 recall 池 **rank 9/60**，融合分 0.4537 → **过站(b)**。
- 站(c)：终排 BM25 rank **29/60**（得分 0.0213 vs top10≈0.05-0.07）→ **沉底出局**。
  但注意 D14:17（"our true potential…"）是 `likely→potential` 的假阳性，**不是金标**。
- 金标 D15:28 文本："I'm a fan of both classical like Bach and Mozart, as well as
  modern music like Ed Sheeran's 'Perfect'"——**不含 vivaldi/seasons/four 任何查询词**
  （这三个词全语料 df=0），是推理题；词库对这类题结构上无能为力。

小结：两题演示了 miss 的两种死法——q19 死在站(a)（义项错配+词形错位，候选未产生）；
q64 死在站(c) 的候选本身是假阳性（df>0 的扩展词是噪声桥），而真金标在词面层面够不着。

### 第 4 层（判决性实验）：终排墙验证——**L2 无正向信号，终排墙不是根因**

> 说明：本实验是应调查需求追加的判决实验，不受"L1 无信号即停"纪律限制。

A/B（sample50 基线模式；L1 = wordnet 灌库 + --extractor-dict）：

| 对照 | 覆盖率 | Δ | 逐题 |
|---|---|---|---|
| L1 vs L1+L2（--final-alpha 0.6 --final-beta 0.4） | 0.443 vs 0.432 | **-0.010** | q102 1.0→0.0、q14 0.5→1.0（2 题翻转） |
| off vs L1+L2 | 0.448 vs 0.432 | -0.016 | 3 题翻转 |

判读：终排混合未让覆盖率明显动（更谈不上 >+0.02），方向甚至略负——**语义候选
结构性沉底的"终排墙"假说不成立**；融合分顶上来的候选（如 q64 的 D14:17 类假阳性）
不是金标。回第 1/2/3 层，证据链已闭合。

### 根因结论（一段话）

词库端到端无效不是管道断点（第 1 层：10.4 万别名/1,956 边真实在库；第 2 层：
45/50 题扩展触发、硬种子通道 41/50 题正常），也不是终排墙（第 4 层：混合终排无信号）。
**根因是资产与语料的词形-词义分布错位**：wordnet 词库是 WordNet 书面 lemma 同义词，
义项偏向正式/形容词词义（like 的同义是 comparable 而非 love），口语核心词对
（kids/children）直接缺失，且全链路精确词形匹配（loves/loved 命不中 lemma love）
——于是扩展词在口语语料中 df≈0（114 词仅 17 有 df>0，且多为 years→days 类噪声桥），
语义候选要么在收录门之前不产生（q19），产生的是假阳性（q64），端到端覆盖率自然不动。

### 处置建议（更新 §8）

维持"**移出评测管线默认流程**"，依据较 §8 更硬：结构存在、激活触发、各站工作正常
而端到端零收益，证明无效性在资产内容本身，激活设计（查询侧词典、混合终排）无法补救。
若未来要重建资产方向：(a) 从语料词表出发蒸馏同义词（而非 WordNet 全量灌入）；
(b) 词形归一（stemming/lemma）接入 M3 或 lex 通道切词；(c) near 词纳入默认扩展。
此为资产重建，不是开关修复；`--extractor-dict` 开关代码保留（默认关闭），中文场景
（CJK 通道）仍是唯一未验证的复用方向。

### 补记：ctest 结果

`ctest` **20/20 全绿**（总 9.13s；含链接改动后 `libame_bench` 的 test_bench 5.51s 通过、
test_extractor 33/33 子项通过）。注：本轮曾因 Windows 环境偶发问题（DLL 路径 + 上一轮被
超时打断的残留进程）出现过 test_storage 段错误/test_extractor 挂起，干净环境下重跑
全绿，与代码改动无关。

### 补记：pristine 双跑（无改动二进制）

将 4 个改动文件 `git stash` 后重build，同一 off 命令连跑两次：
**dump 不一致，覆盖率 0.448→0.443（Δ=-0.005，q32 翻转）——与带改动二进制完全同款的
噪声签名**。基线固有非确定性定论，"逐字节一致"回归口径对该管线不可达，§4.1 口径成立。

---

## 10. 英文蒸馏：语料蒸馏词库的重建审判（2026-10-15 追加）

> 动机：§9 根因显示 wordnet 资产无效的原因是"书面 lemma 与口语语料错位"。
> 本节按"语料蒸馏、对口优先"重建英文词库，给词库路线一次完整的公平审判。

### 10.1 词表（bench/data/en_vocab.txt）

- 来源：`locomo10.json`（×20 加权，评测语料保护）+ 3 个 fixtures + `longmemeval_s_cleaned.json`
  （500 题，2,090 万词元）。格式沿用 `build_lexicon_json` 输入契约
  （`{"conv_vocab_top":[[w,f],...]}` JSON——任务书"一词一行 txt"与代码契约不一致，
  以代码契约为准，扩展名保留 .txt）。
- 过滤：纯 ASCII 小写词、len≥4、停用词（lex 通道 43 + 功能词；**不含** M1 提取停用词——
  `like` 在问题中是真实内容词，M1 停用词表只作用于提取器不作用于 lex 通道）。
- 规模：**12,127 词**（top 12,000 + locomo 原始词频≥3 覆盖保护 127 词）。
- sample50 问题内容词 122 个，覆盖 113；缺失 9 个（3 专有名词、2 问题侧拼写错误
  educaton/persue、4 个语料词频=1 的词），均为边际词。

### 10.2 蒸馏（复用 build_lexicon_json，零 prompt 改动）

- 前提验证：(a) API 健康探测通过（topn=2 冒烟，1 调用成功）；(b) 现有 prompt 本身即
  英文导向（"为下列每个**英文**词生成词典条目"，syn_strong 约定含词形变化
  painting→paint）——口语桥 like→[enjoy, **love**, fancy, prefer] 冒烟即得，
  无需英文 prompt 变体。
- 产出：`lexicon/lexicon_en.json` **12,127 条**（924KB；有 syn_strong 9,546、
  syn_near 10,138、antonyms 6,217 流非空）。
- 成本：首跑 383 次 API 调用 / 6m32s（243 批 + 截断翻倍重试，失败批 0，断点落盘）；
  修复落盘 bug 后增量续跑 143 批全部缓存命中、0 新增调用。
- **过程中修复真 bug**：`llm/src/lexicon.cpp` `write_lexicon` 的
  `std::rename` 在 Windows 不可覆盖已存在目标 → 多 chunk 蒸馏每轮落盘静默失败、
  out 文件永远落后一轮（本次首跑 12,127 条只落上 5,000 条）。修复为先删再改名；
  该 bug 影响所有 Windows 上的多 chunk 蒸馏续跑，建议入主线。

### 10.3 L2 层激活判决（预注册：≥60/219 有效）

灌 `lexicon_en.json` 后复跑 dbg_lextrace 第 2 层调查：

| 指标 | wordnet | en 蒸馏 |
|---|---|---|
| 别名表 | 103,905 | 9,413（applied=12,665） |
| REL_SIMILAR / OPPOSITE 边 | 1,892 / 64 | 576 / 258 |
| 有 variants 的内容词 | 114/219 | 71/219 |
| **扩展词 df>0** | **17/219** | **17/219** |

**预注册判决：未达 ≥60/219，词库路线按预注册判整体证伪。**

但调查同时定位到一个压制蒸馏资产的管道断点：`apply_lexicon` 的 `alias_ok`
守卫（防 wordnet 多义词链式改写设计）拒绝"同义词本身也是主词且不互指"的别名对——
恰好吃掉蒸馏产物里最高频的口语桥 **8,094/20,173（40%）**：like→love/enjoy/fancy/
prefer、great→excellent/wonderful、make→produce/build、kids→children 全部被拒。
即 L2 指标测量的是"被守卫截肢后的资产"。按预注册纪律不重开实验翻案（避免移动门柱），
将该断点记录为词库路线若要复活的唯一工程前提。

### 10.4 端到端 A/B（sample50 基线模式，命令同 §4，--load-lexicon lexicon/lexicon_en.json）

| 组 | 覆盖率 | Δ vs off | 逐题 |
|---|---|---|---|
| off | 0.448 | — | — |
| en 灌库 | 0.443 | -0.005（噪声） | q32 翻转（已知噪声题） |
| en 灌库 + extractor-dict（en_words.txt 28,068 词） | 0.443 | -0.005（噪声） | 与 en 灌库组逐题全同（+0.000） |
| en 灌库 + dict + L2 混合终排 | 0.432 | -0.016 | q102↓/q14↑/q32↓（与 wordnet 版 L1+L2 完全同款翻转） |

端到端无正信号，与 L2 层预注册判决一致。extractor-dict 再度逐题零差异——
与 §3 结论一致：英文自由词元规则已覆盖 len≥4 词，大词表在查询侧没有边际。

### 10.5 抽样质检（seed=42，随机 20 条，人工判读）

```
knead      | strong: massage,work            | near: manipulate:0.8,press:0.7,mix:0.6        | ant: -
electric   | strong: -                       | near: electrical:0.9,dynamic:0.6               | ant: -
size       | strong: dimension,magnitude     | near: proportion:0.8,scale:0.75                | ant: -
optimizing | strong: perfecting,improving    | near: enhancing:0.8,refining:0.8               | ant: worsening,deteriorating
homeless   | strong: unhoused,vagrant        | near: destitute:0.7,displaced:0.6              | ant: housed,settled
equality   | strong: parity,sameness         | near: fairness:0.7,uniformity:0.6              | ant: inequality,disparity
sushi      | strong: -                       | near: sashimi:0.7,maki:0.7,nigiri:0.7          | ant: -
cherishing | strong: treasuring,prizing      | near: adoring:0.8,valuing:0.75                 | ant: neglecting,despising
managing   | strong: administering,directing | near: controlling:0.8,coping:0.7               | ant: mismanaging
fave       | strong: favorite,preferred      | near: beloved:0.8,cherished:0.75               | ant: least favorite,disliked
insecticidal| strong: pesticidal,bug-killing | near: herbicidal:0.6,fungicidal:0.6            | ant: -
giving     | strong: give,donating,presenting| near: offering:0.8,providing:0.8               | ant: taking,receiving,withholding
sequoia    | strong: -                       | near: redwood:0.8,giant sequoia:0.9            | ant: -
dispute    | strong: argue,debate            | near: contend:0.8,quarrel:0.7,challenge:0.7    | ant: agree,concur
regular    | strong: normal,standard,usual   | near: common:0.8,routine:0.7                   | ant: irregular,abnormal,unusual
month      | strong: -                       | near: period:0.6,term:0.5                      | ant: -
four       | strong: -                       | near: -                                         | ant: -
garage     | strong: -                       | near: carport:0.7,parking:0.6                  | ant: -
chord      | strong: -                       | near: harmony:0.7,note:0.6,triad:0.6           | ant: -
neil       | strong: -                       | near: -                                         | ant: -
```

判读：三流区分基本合理（strong 可互换/含词形变化：giving→give、fave→favorite；
near 不可互换：garage→carport；ant 方向正确：dispute↔agree、homeless↔housed）；
词形变化覆盖良好（cherishing/optimizing/managing 的 -ing 形→lemma 或同义 -ing）。
噪声存在但低：knead→massage 偏松、sequoia 自环 near（giant sequoia）、
个別数字/专名空词条（four/neil，符合 prompt 约定）。**资产质量本身合格——
问题不在蒸馏，在灌库守卫与匹配方式。**

### 10.6 最终判决：词库路线整体证伪（两次独立证伪，机制完备）

- 重建审判结论：语料蒸馏资产质量合格（§10.5）、口语桥真实存在（like→love），
  但 (a) `alias_ok` 守卫吃掉 40% 高频桥（§10.3），(b) 全链路 lemma 精确匹配
  过不了 loves/loved 词形（§9 第 3 层），(c) 终排混合无信号（§10.4）。
- 按预注册纪律：L2 指标 17/219 < 60 → **词库路线（蒸馏版）整体证伪，不再投**。
- 资产处置：`lexicon_en.json` 与生成脚本保留入库（lexicon/README.md 资产总表可补一行）；
  评测管线默认流程不带任何英文词库。若未来复活，工程前提有二：
  ① `alias_ok` 守卫需区分"蒸馏互指"与"wordnet 多义"（如对蒸馏源放行主词对）；
  ② M3/lex 通道需词形归一（loves→love）。
- 附带主线建议：`write_lexicon` Windows rename 覆盖 bug 修复（§10.2）与
  本任务无关但影响所有蒸馏续跑，建议随其它改动一并提交。

### 改动清单（本节）

- `llm/src/lexicon.cpp`：write_lexicon 落盘 Windows rename 覆盖修复（真 bug）。
- `bench/scripts/gen_en_vocab.py`（新）：英文词频表生成（多语料加权 + 评测覆盖保护）。
- `bench/scripts/gen_wordnet_words.py`：泛化为可传 src/out 参数（默认行为不变）。
- `bench/data/en_vocab.txt`（新，12,127 词）、`bench/data/en_words.txt`（新，28,068 词）、
  `lexicon/lexicon_en.json`（新，12,127 条）——入库；`*.ac`/bench/cache 不入库。
- `bench/scripts/dbg_lextrace.cpp`：沿用（无改动）。
- 运行产物：`bench/runs/lexicon_ab/{en,en_dict,en_dict_L2}.*`、`bench/runs/lexicon_en_distill{,2}.log`。
- ctest **20/20 全绿**（llm 改动后全量 rebuild + ctest，11.82s）。

---

## 11. 复活工程（R1 守卫放行 + R2 词形归一）与第三次审判：词库路线永久封存

> 前置（§9/§10）：蒸馏资产质量"抽样"合格，但 alias_ok 守卫拒收 40% 高频口语桥、
> 全链路精确词形匹配过不了 loves/love。本节拆这两道闸后做第三次（最后一次）审判。

### 11.1 R1：守卫放行（蒸馏源专用）

- 守卫语义复查：`alias_ok` 防的是"同义词本身也是主词且不互指"的单向归并（多义词
  链式稀释，如 great↔big 互指时只放行互为成员的对）。**中文 hf 资产同被此守卫吃掉**：
  抽样 16,068 对 strong 有 5,299（33%）被拒——"中文资产有效"同样从未被端到端验证，
  一并披露（此前 README/报告的说法仅为灌入幂等观察，非效果验证）。
- 设计：lexicon JSON 可选 `"source"` 字段（`lexicon_en.json` 已标 `"distilled"`）；
  灌库时蒸馏源 syn_strong 全量走新入口 `KeywordManager::add_alias_distilled`
  （canonical=词条主词，另防"双向互指成环"：standard 已归一到 alias 时拒登）。
  无 source 字段的源（wordnet/中文）走原 `alias_ok` 守卫路径，**逐比特不变**（单测锁定）。
- 效果：别名表 9,413 → **11,926**（applied 12,665 → 19,231，40% 桥入表）；
  逐题验证 like→love / kids→children 桥真实生效（§11.3）。

### 11.2 R2：词形归一（查询/写入双侧共用）

- `KeywordManager::normalize` 增加英文保守词干（`set_stem_en` 开关，**默认关**）：
  别名未命中的纯 ASCII 词按序产出候选词干（'s/ies→y/ied→y/e+d 只去 d/去 ed+双写还原/
  去 ing+双写还原/ses 双候选/s(x|z|ch|sh)es→去 es/去 s），**护栏=候选词干在图中
  有 CONTAINS 事件连接（df>0）才归一**，通过后仍查一次别名表（politics→politic→political
  全链贯通）。CJK 与非 ASCII 词不受影响。
- 两侧对称性：normalize 是写入侧（M4 CONTAINS 落边）与查询侧（M5 自由文本种子、
  R1 后 lex 通道查询词先归一再取别名组）共用的唯一入口，天然对称，无单侧捷径。
- 摄入侧效果：conv-26 Keyword 节点 1163 → **1046**（词形合并生效）。
- 单测（test_keyword 26 项 / test_bench 101 项全绿）：loves/LOVES→love、loved→love
  （e+d 特判防 made→mak）、senses→sense 且 sense 不过度、politics→political、
  防互换成环、默认关闭 = 历史行为、distilled/wordnet 双源守卫行为锁定。

### 11.3 第三次审判（预注册判决线）

**L2 层（df>0 ≥60/219 才算资产有效）**：R1+R2 后扩展词 df>0 = **17/219**（与 wordnet、
守卫版 en 三次同值）——未达线。逐题看桥是通的（q19 实测 kids→youngsters/children、
like→love 均 df>0），但聚合指标未动：多数查询词的真同义确实不在 419 事件的小语料里，
df>0 对小语料天然苛刻。**预注册判决：未达线。**

**端到端（off 0.448 对照，>+0.02 复活 / ±0.02 内无信号封存）**：

| 组 | 覆盖率 | Δ vs off | 逐题 |
|---|---|---|---|
| off | 0.448 | — | — |
| en+R1+R2+extractor-dict（--stem-en） | 0.443 | **-0.005** | 仅 q32（已知噪声翻转题） |
| 同上 + L2 混合终排 | 0.432 | -0.016 | q102↓/q14↑/q32↓（与历次 L2 完全同款翻转） |

判决：**无信号 → 词库路线永久封存**（三次独立证伪：wordnet 资产、守卫版蒸馏资产、
拆闸版蒸馏资产；判决线两次预注册、一次端到端，全部未达）。

### 11.4 为什么桥通了端到端还是不动（最终归因，调查闭环）

1. **lex 收录门**：单桥候选过不去 m≥2/单罕见词收录线（q19 金标 D6:6 仅靠
   love 单命中，与 §9 第 3 层同一死法）；
2. **R1 放行暴露资产真面目**（蓝方抽检，100 个语料词）：normalize 改变 53 词中
   **仅 4 个是屈折还原，~46 个是松同义重定向**（dogs→pooches、started→inception、
   cakes→brownies、valued→appraised、keeps→retain）——蒸馏 strong 流大量不符合
   "可互换真同义"标准，**alias_ok 守卫不是误伤而是必要过滤**；§10.5 的 20 条随机
   质检"合格"是抽样偏差，高频动词才是污染重灾区；
3. 终排混合（L2）四次实验方向均略负，融合分顶上的不是金标。

**最终结论：词库资产假说在本 benchmark 上整体证伪并永久封存。** 蒸馏管线
（gen_en_vocab.py / build_lexicon_json）与资产文件保留存档；`--extractor-dict`、
`--stem-en`、`--final-alpha/--final-beta` 开关保留（默认全关，行为与基线一致）；
若未来出现更大语料/别的 benchmark 想重启，必须先解决"strong 流质量"（可互换性校验）
与"lex 收录门"两个前置，否则不要再投。

### 蓝方自检逐条

1. **归一函数两侧对称性**：✅ normalize 为写入/查询唯一共用入口；词干护栏基于图状态，
   两侧同一时刻同一图，无单侧捷径。顺序依赖（先到的词形自建节点）已披露，benchmark
   摄入序确定故可复现。
2. **守卫放行的多义污染样本观测**：✅ 100 词抽检 ~46% 松同义重定向（§11.4-2），
   这是本次调查最重要发现——守卫被平反。
3. **词干护栏误伤率**：✅ 100 词中词干仅触发 4 例且全部正确（birthdays→birthday、
   friendships→friendship、happens→happen、brightening→brighten）；拒收案例合理
   （campaigns 因 campaign 节点无 CONTAINS 连接保守不并）。过度截取（made→mak/
   senses→sens）由"不做 e-还原 + df 护栏"双重防住，单测锁定。

### 改动清单（本节）

- `keyword/include/.../keyword_manager.h` + `keyword/src/keyword_manager.cpp`：
  add_alias_distilled（防互换成环）、en_stem 候选生成 + stem_df_ 护栏、
  set_stem_en 开关、normalize 集成（默认关 = 历史行为）。
- `bench/src/lexicon_apply.cpp`：source=="distilled" 全量放行，其余源守卫逐比特不变。
- `seed/src/seed_generator.cpp`：lex 通道查询词先 normalize 再取 variants/near。
- `bench/cli/main.cpp`：`--stem-en` 裸开关（修复过程中发现其误吞下一参数的解析 bug）。
- `lexicon/lexicon_en.json`：加 `"source":"distilled"` 根字段。
- `tests/test_keyword.cpp`（+test_stem_en 26 项）、`tests/test_bench.cpp`
  （+test_apply_lexicon_source_guard 101 项）。
- `bench/scripts/dbg_lextrace.cpp`：默认开 stem + `--stemcheck` 蓝方抽检模式。
- ctest **20/20 全绿**（8.88s）；E2E 运行产物 `bench/runs/lexicon_ab/R1R2{,_L2}.*`、
  `bench/runs/stem_{sample,out}.txt`。

---

## 12. 第四轮：换真机制（M1 收录门分级 + M2 strong 流精筛）与终局审判

> 第三轮封存的两个真死因：lex 收录门 m≥2 挡单桥候选；strong 流 46% 松同义。
> 本轮分别用 M1/M2 拆这两道闸，审判线与第三轮同口径可比。用户指定为最后一次。

### 12.1 M1：收录门分级（seed_generator lex 通道，`--bridge-admit` 默认关）

- 直接命中计 1.0（原语义）；**桥介命中**（事件词经 M3 归一——strong 别名表/词干——
  后命中查询词面，且归一必须真实改变词形 = 防噪声闸）计 0.5；
- 收录线：总分 ≥2.0 维持原行为；**≥1.0 且含 ≥1 桥介 → 收录但权重 (0.3+0.7·norm)×0.5、
  norm 封顶 0.5**（norm = 命中分/idf_sum）。
- 单测 test_bridge_admit（24/24 全绿子集）：q19 型（kid 直接 + loved→love→like 纯桥介，
  该形不在 variants 扩展表内）开关关不进池、开关开进池且权重 ≤0.33；零命中噪声事件
  零新增；默认关 = 历史行为（test_seed 既有断言全破不了）。

### 12.2 M2：strong 流可互换性精筛（`--validate-lexicon`，llm 层新函数）

- 逐对问 LLM"大多数句子里 A 能否直接替换 B 且句意不变"，批量 50/批、磁盘缓存、
  断点续跑、失败批 fail-open（宁留噪声不丢真同义，缓存使重跑零成本）。
- 成本：**404 次调用 / 1m29s / 失败批 0**（20,173 对）。
- 产出 `lexicon/lexicon_en_validated.json`（不动原始蒸馏产物；root 带
  source=distilled, validated=true）：**strong 对 20,173 → 9,419（保留 46.7%）**。
- 质检（随机 20 条被拒对，人工判读）：拒绝基本正确——switch→replace、apartments→
  condominiums、picnic→cookout、exist→be、gospel→doctrine 确实不可直接替换；
  need→demand、town→village、uncommon→rare 偏严但在"直接替换"语义下可辩护。
- **重要观察**：冒烟时 LLM 把 **like→love 判 no**（"I like ice cream"→"love" 语义强度
  变了）——严格可互换语义会系统性拒绝"召回有用的方向性近义桥"。M2 按任务书原样执行
  （预注册），但该观察说明：召回场景要的是方向性宽松语义（本质就是 syn_near 0.6 折
  扣扩展做的事），不是双向可互换。

### 12.3 审判 A/B（预注册线）

**L2 层 df>0**：validated 词库 **10/219**（<60 预注册线，未达；比守卫版 17 还低——
精筛把"有 df 的松同义对"也删了）。

**端到端（off 0.448）**：

| 组 | 覆盖率 | Δ vs off | 关键翻转 |
|---|---|---|---|
| B = validated+stem+M1 | 0.464 | +0.016 | **q22 0→1.0**（稳定） |
| C = B+extractor-dict | 0.469（峰值）/ 0.464（两次复跑稳定） | +0.021 峰值 / **+0.016 稳定** | q22 0→1.0；any-hit 25→26 |
| 无 M1 对照（validated+stem） | 0.464 | +0.016 | 与 B 逐题全同 |

**单桥 miss 救回线**：**过**——q22（"Would Caroline likely have Dr. Seuss books…"，
would-likely 推理类单桥 miss）金标 D6:9 进 top-k（rank 5），三次运行稳定。
q19/q64 本体仍 miss（D6:6 仅 love 单桥 0.5 < 1.0 收录线；D15:28 无任何查询词面）。

**逐项判决**：
1. L2 df>0 ≥60/219：**不过**（10/219）；
2. C 组 >+0.02：**不过**（稳定 +0.016；+0.021 为单翻转题的噪声上沿）；
3. 单桥类 ≥1 题金标进 top-k：**过**（q22，稳定 ×3）。

### 12.4 蓝方自检

- **桥介收录噪声实测**：M1 开 vs 关在 sample50 top-k 上**零新增候选**（逐题 50/50
  一致）——桥介候选进了种子/扩散但在终排前未达 top-k；零噪声、零收益（本组数据）。
  q22 的救回来自 validated 词库+词干引起的图结构变化（D6:9 词面含 books 直命中 +
  关键词图拓扑变化），非桥介通道。
- **M2 精筛方向观察**：严格可互换语义系统性拒绝方向性近义桥（like→love=no），
  召回场景的语义宽松度需求与"防污染"天然张力——这是词库路线的深层悖论，不是
  工程能完全消除的。
- **机制归属诚实声明**：q22 救回为真实稳定收益，但归因于"validated 灌库+stem"组合
  效应，无法干净拆到单一开关。

### 12.5 终局结论（第四次，用户指定的最后一次）

预注册判决线 2/3 未过（决定性复活线 C>+0.02 稳定值 +0.016 未达）→ **词库路线
在本 benchmark 不复活，也不再续投**。与第三轮"结构性承载不了"的措辞不同，本轮数据
给出了更精确的终局画像：**词面范式对词级语义有真实但极有限的承载力**——四轮工程
把覆盖率从 0.448 推到稳定 0.464（+0.016，≈1 题/50 的机制性救回、any-hit +1），
这就是该范式在本 benchmark 上的收益上限。要再往上，缺的不再是别名录入与词形归一
（两道闸都拆了、资产也精筛了），而是语义召回本身（向量/推理通道）的事。

### 改动清单（本节）

- `seed/include/.../seed_generator.h` + `seed/src/seed_generator.cpp`：
  SeedQuery.bridge_admit + lex 通道桥介收录分级（默认关 = 历史行为）。
- `llm/include/.../components.h` + `llm/src/lexicon.cpp`：
  `validate_lexicon_json`（M2，批量/缓存/断点/fail-open）。
- `bench/cli/main.cpp`：`--bridge-admit` 裸开关、`--validate-lexicon` 模式、usage。
- `bench/include/.../bench.h` + `bench/src/ingest_answer.cpp`：cfg.bridge_admit 透传。
- `tests/test_seed.cpp`：+test_bridge_admit（24/24）。
- 新资产：`lexicon/lexicon_en_validated.json`（12,131 条，strong 9,419 对，已标
  validated=true）；原始 `lexicon_en.json` 未动。
- ctest **20/20 全绿**（11.39s，llm 层改动后全量 rebuild）。
- 运行产物：`bench/runs/lexicon_ab/{B_valid,C_valid_dict,Bprime_valid_nobridge,
  C_rerun1,C_rerun2}.*`、`bench/runs/lexicon_en_validate.log`。
