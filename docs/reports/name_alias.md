# 人名/专名归一域：姓名别名表设计、实体侧贯通与中文场景验收

> 任务分支：names-line（基于 dev-2.0 的 917e9aa）。前置结论：词库六轮调查
> （lexicon_activation.md §9–§14）判定等价归一表的正确域是**专有名词**（人名/昵称/
> 地名/机构名的同一性归并，严等价+守卫必要），普通词同义属 graded 边表（第六轮已上线，
> bench 收益≈0）。本报告让归一表在它的正确域干活。

## 1. 设计

### 1.1 数据源 `lexicon/names_zh.json`（自建 schema）

```json
{"source": "names_zh",
 "groups": [{"canonical": "李建国", "aliases": ["小李", "李经理", "建国"]},
            {"canonical": "王秀英", "aliases": ["小王", "王姐", "秀英"]},
            {"canonical": "陈志强", "aliases": ["老陈", "小陈", "志强"]}]}
```

- **语义**：组内是人工确认的"同一自然人"断言（严等价，不是 graded 相似）。守卫语义
  由 `KeywordManager::add_alias` 内置保护原样承担（内置标准词不可被改写、
  alias==standard 跳过）——六轮报告结论：这个域守卫是对的，不动。
- **同名同姓两人**：不得进同组。同一性判定是表的维护职责（数据侧），引擎侧不做
  二次判定（蓝方 §5 第 1 条）。

### 1.2 一处数据、三通道贯通（`apply_name_aliases`，摄入前加载）

与 `--load-lexicon`（摄入后灌库）不同，`--name-alias` 必须在**摄入前**加载——写入侧
normalize 依赖别名表：

| 通道 | 落点 | 作用 |
|---|---|---|
| M3 别名表 | `keywords().add_alias(alias, canonical)` | 写入/查询两侧 normalize 严等价归并（守卫原样） |
| M1 提取器 | `extractor().add_keyword(词面, "concept")`（canonical+全变体） | 非称呼型变体（李经理/建国）不经注册无法从原文提取；注册后摄入时提取→normalize→canonical 关键词节点（CONTAINS 通道统一） |
| M2 实体 | `register_entity("person", canonical, {}, aliases)`（幂等：已建档跳过） | canonical 建档 + 全变体 Alias 节点；摄入时称呼模式（老X/小X）提取的昵称实体经 M2 三级候选的 **Alias 命中**归并到 canonical 实体（INVOLVES 通道统一） |

另有两处引擎侧"衔接件"：

- **摄入侧**（`Engine::remember_event`）：实体 surface 命中别名表（新增
  `KeywordManager::alias_of` 精确查询，区别于 normalize 的小写化）时，事件补挂
  canonical 关键词——称呼轮（如"小李说……"）原本只有 INVOLVES 实体锚点，补挂后
  CONTAINS 通道对称呼轮/非称呼轮一视同仁。
- **查询侧**（`SeedGenerator::gen_seeds`）：文本提取出的实体词面经 `alias_of` 命中时，
  ① canonical 实体/关键词节点入硬种子；② **canonical 词面注入 CJK 词法通道的内容词
  集合**——事件按全变体子串直接成事件级种子（含 variants 扩展）。②是主通道：
  调查中发现 CONTAINS/INVOLVES 的扩散传导弱（出度归一 + 类型系数），纯节点种子
  几乎传不到事件；事件级子串种子（lex 权重）才真正圈定"李建国"的全部轮次。

### 1.3 CLI

`--name-alias <path>`（默认关）。graph 后端每 case 摄入前灌入；lmem 后端按 1.x 专属
开关纪律忽略（有提示）。幂等：重复加载不重复建档。

### 1.4 为什么 `alias_of` 而不是复用 `normalize`

`normalize` 对未命中别名表的词仍做 ASCII 小写化（"Alice"→"alice"），无法区分
"小写化"与"映射"。若用 normalize 判定，英文人名实体会被误当映射、给查询注入多余
关键词种子（sample50 回归风险）。`alias_of` 只认表内键：未加载姓名表时必返回空串，
**所有新代码路径在 off 时零行为差**。

## 2. 实体侧贯通（任务 2）确认结论

读代码确认：**改造前不通**。M2 `resolve()` 只做实体名/Alias 节点精确匹配，从不查
M3 别名表；称呼提取出的"小李"会建成独立碎片实体，与"李建国"的 INVOLVES 边分裂。

接通后（本报告的改动）：

- 摄入："小李说……" → M1 称呼模式提取实体"小李" → M2 resolve 第 3 级扫 Alias 节点
  命中（加载期注册）→ 归并 canonical 实体"李建国"（实测 vote 0.5→0.8，pending 但 uid
  正确），事件 INVOLVES 挂到同一人。
- 查询："小李说过什么" → gen_seeds 桥 → canonical 实体（INVOLVES 通道）+ 全变体轮次
  事件级种子（CONTAINS/lex 通道）→ 圈定"李建国"全部事件。
- 单元锁定：`tests/test_bench.cpp test_apply_name_aliases`（摄入归并断言）、
  `tests/test_seed.cpp test_name_alias_bridge`（双通道种子断言）。

## 3. 中文夹具 `bench/fixtures/names_zh_mini.json` 与判题

### 3.1 为什么不用 LoCoMo

LoCoMo 是英文语料，无中文人名变体，第六轮已证明它测不出专名归一（词级语义总账
<0.02）。自建中文小夹具：3 个对话（李建国/王秀英/陈志强），每对话 2 会话 × 12 轮，
每人 2 题共 6 题。**内容词刻意简单（builtin 词典词），discriminative 全在专名变体**：
对话里全名/称呼/职位称呼混用（李建国/小李/李经理/建国），问题用其中一个变体问，
金标是别的变体的轮次。

### 3.2 防泄漏设计（蓝方级，OFF 臂必须真 0）

OFF 臂金标不能被任何既有通道捡到，否则 0→非0 判据失效。四条规则：

1. **问题必含一个 builtin 词典词 W**（开会/跑步/聚餐/体检）——CJK 提取通道非空时
   二元组兜底通道被抑制，切掉"问题与金标共享二元组"的泄漏路径。
2. **金标轮不含 W，且与所有 W 轮 NEXT 距离 ≥3**（跨会话链计距）——W 关键词种子的
   深度 2 扩散够不到金标。
3. **问题变体不在对话中出现**（或仅以非称呼形式出现——OFF 臂不可提取则惰性）；
   称呼型变体（老X/小X）在对话出现时会建碎片实体，故问题的称呼变体必须对话零出现。
4. **问题与金标文本共享汉字 ≤2 个**——β·context（哈希字符 n-gram 余弦）是最强残余
   泄漏源（实测共享 3-4 字即可把金标推进 top-10），用措辞控制到 1-2 字后 OFF 臂 0/6。
   该泄漏对两臂对称存在，ON 臂靠硬种子高分压过。

W 诱饵轮（每 case 3-4 处）兼做自然性与"OFF 臂候选数 ≥10 把金标挤出 top-k"的填充。

### 3.3 判题口径

`--judge f1` 基线模式，判据看 **recall@k（金标 dia_id ∈ evidence_dia_ids）**。
中文问题无 ASCII 内容词 → 内容词终排恒 0 → 弃答门控普遍弃答（abstain=1.000），
答案文本 F1 无意义（ON 臂 F1≈0.094 仅参考）；弃答不影响证据 dia_id 记录与覆盖判定。

## 4. 对照表与判决（预注册：OFF 0 → ON 非 0，几道题全对）

命令（可复跑，`export PATH="$(dirname $(which g++)):$PATH"` 前置）：

```bash
# OFF
./build-win/bench/ame_bench.exe --dataset locomo --data bench/fixtures/names_zh_mini.json \
  --extractor rule --answer baseline --judge f1 --k 10 --dump bench/runs/name_alias/OFF.ndjson
# ON = OFF + --name-alias lexicon/names_zh.json
```

| 运行 | recall@k（n=6） | 逐题 |
|---|---|---|
| OFF | **0.000（0/6）** | 6 题金标全部不进 top-10 |
| ON  | **1.000（6/6）** | 6 题金标全部命中 |
| OFF 复跑 | 0.000 | 稳定 |
| ON 复跑 | 1.000 | 稳定 |

**判决：预注册判据达成**——归一表在专名域从"完全不干活"到"全对"，开关纯增量。

### 样本输出（ON 臂，dump 摘录）

**names-1:q0** — 对话混用："李建国说明天要开会讨论预算。"(D1:1) / "建国答应周三前给新数字。"(D1:5) / "小李说数字要砍三成。"(D1:7) / "李经理当场拍板砍半。"(D2:1)；问题用变体"李经理"问，金标是"建国"轮：

```
Q: 李经理开会时定了预算的事吗？
gold: D1:5
evidence: [D1:1, D2:1, D2:7, D1:9, D2:5, D1:7, D1:5*, D2:3, D1:8, D2:6]  (*金标命中)
```

**names-2:q1** — 问题用变体"秀英"，金标是 canonical 轮"王秀英昨天刷了十公里。"(D2:1)：
证据序 [D1:1, D2:1*, D1:5, D2:5, D1:4, D1:2, D1:3, D2:2, D1:12, D2:6]。

**names-3:q1** — 问题用称呼变体"老陈"，金标是"志强那辆车出毛病了……"(D2:1)；
对话里"老陈"(D1:5)/"小陈"(D2:5) 称呼轮摄入时均归并 canonical 实体：
证据序 [D1:1, D1:5, D2:9, D2:5, D1:9, D2:1*, D1:2, D2:2, D1:3, D2:10]。

## 5. sample50 零回归（纯增量确认）

命令 = §3 命令换成 `--data D:/agent/memory/ame/bench/data/locomo10.json
--question-ids bench/data/locomo_sample50.json`（locomo10.json 为 gitignore 大数据，
只读引用主工作区，未写入）+ ON 臂加 `--name-alias lexicon/names_zh.json`：

| 运行 | recall@k（n=50） |
|---|---|
| OFF #1 / #2 / #3 | 0.500 / 0.500 / 0.500 |
| ON（name-alias） | 0.500 |

- **逐题覆盖四臂全一致**（逐题 hit 指示符零差异）；证据集合差异 = 已知噪声带内同分
  翻转（q109 型：同覆盖、同分候选次序翻转），与 lexicon_activation.md §4.1 记录的
  基线固有噪声同源（gen_uid 随机播种 → 哈希迭代序 → 同分次序翻转）。
- 机制上零回归由构造保证：所有新代码路径以 `alias_of` 命中为闸，姓名表只含中文
  专名，英文语料无任何表面命中；`remember_event` 的 Extraction 值副本在 alias_of
  未命中时与原文逐字段一致。
- 备注：本分支复跑 OFF 覆盖 0.500，与 lexicon_activation.md 记录的 0.448 基线不同
  （历史报告基于另一分支快照，非本次改动引入；本次 ON/OFF 对照用同二进制同分支，
  控制变量成立）。

## 6. 蓝方自检逐条

1. **守卫对姓名域的误伤面（同名同姓两人）**：同名同姓两人进同组会被强制归并——
   这是严等价表的固有语义，由表维护者负责（数据不进组即零误伤）；引擎侧 M2 投票
   在多候选时降级（pending/suspected_dup），不会静默合并两个已存在的同名实体。
   `alias_of` 精确查询避免把英文人名小写化误判为映射，守卫误伤面只含表内键。
2. **称呼规则与别名表的优先级**：摄入时称呼提取（老X/小X）产出实体 surface →
   M2 resolve 先查实体名再查 Alias 节点（加载期注册）→ 命中即归并 canonical，
   不建碎片实体；碎片实体仅在表未加载时产生。查询时称呼 surface 优先按别名表归
   canonical（alias_of 闸），未命中退回实体精确匹配——优先级确定且可测。
3. **夹具不泄露答案进问题**：金标轮内容词（新数字/砍半/十公里/摩托）刻意不与问题
   共享（§3.2 规则 4），问题只提供"哪个人的哪类事"的圈定，答案信息全在金标轮——
   判别力确实全在专名变体上；6 题 OFF 全 miss 反证无泄漏。
4. **OFF 臂残余通道排查**：ASCII 词法（中文无词元）、实体通道（中文人名非 ASCII
   大写词，永不进实体通道）、CJK 字典通道（OFF 臂变体不入词典，仅 W 直达）、
   W 扩散（距离 ≥3）、软向量（共享字 ≤2 时压不出 top-10）——全部封死或实测为 0。
5. **摄入前加载的副作用**：M1 词面注册使 builtin 词典多出 12 个专名词条，仅影响
   含这些词面的文本提取；sample50 英文语料零影响（实测）。
6. **幂等与持久化**：重复 --name-alias 加载不重复建档（find_by_name 幂等闸）；
   引擎快照 save/load 已持久化 M3 别名表与 M2 Alias 节点，load-graph 路径重放
    aliases_from_json 语义不变（本特性新增部分随组合快照走同一机制）。
7. **性能**：alias_of 每实体一次哈希查找；remember_event 一次 Extraction 值副本
   （小结构体向量，纳秒级）；查询侧注入仅别名命中时发生。sample50 四臂耗时无异。

## 7. 改动清单

- `lexicon/names_zh.json`（新增）：3 组人名严等价别名（任务书 schema）。
- `keyword/include+src`：新增 `alias_of` 精确命中查询（与 normalize 区分），
  守卫（add_alias 内置保护）原样未动。
- `engine/src/engine.cpp`：`remember_event` 取 Extraction 值副本；实体 surface
  alias_of 命中时事件补挂 canonical 关键词（CONTAINS 通道对齐 INVOLVES）。
- `seed/src/seed_generator.cpp`：查询侧人名归一桥——alias_of 命中时 canonical
  实体/关键词入硬种子 + canonical 词面注入 CJK 词法通道（事件级种子主通道）。
- `bench/src/lexicon_apply.cpp` + `bench/include/ame/bench/bench.h`：新增
  `apply_name_aliases`（M3+M1+M2 三通道灌库，摄入前调用，幂等）。
- `bench/cli/main.cpp`：`--name-alias` 开关（默认关，摄入前加载，lmem 忽略提示，
  usage 补录）。
- `bench/fixtures/names_zh_mini.json`（新增）：3 对话 6 题中文专名变体夹具。
- `tests`：test_keyword 加 `test_alias_of_and_guard`（守卫原样锁定）；
  test_seed 加 `test_name_alias_bridge`（双通道种子 + 英文零回归）；
  test_bench 加 `test_apply_name_aliases`（三通道贯通 + 摄入归并 + 幂等 + 坏文件）。
- `lexicon/README.md`：补 names_zh.json 专名归一域说明。
- ctest **20/20 全绿**（11.54s/10.39s，含新增断言）。
- 运行产物：`bench/runs/name_alias/{OFF,ON}.ndjson`、`sample50_{OFF1,OFF2,OFF3,ON}.ndjson`。

## 8. 结论

归一表在正确域（人名/专名同一性）一次打通：夹具 OFF 0/6 → ON 6/6（预注册判据达成），
sample50 逐题零回归（纯增量）。机制要点：专名归一的价值不在 graded 相似，而在
**跨词面身份同一性**——这是六轮词库工程证伪"词级语义承载"后，等价归一表保留的
唯一、且必要的生存域。
