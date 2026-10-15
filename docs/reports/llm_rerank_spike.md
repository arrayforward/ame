# LLM 重排通道判决实验（句级语义 spike）

> 前置结论：`lexicon_activation.md` §14 终局——词面范式词级语义承载上限 ≈+0.016，
> sample50 覆盖率 plateau 0.448-0.464；剩余 miss 主体是**句级改述鸿沟**
> （A 类 5 题 + q104 型近失）。句级语义是唯一未试过的层。BGE 服务端方案被成本
> 否决，本 spike 用现有 DeepSeek 客户端 + 磁盘缓存做 LLM 重排，量"句级语义的
> 真实收益上限"。**性质：判决实验（值不值），不是产品形态。**

## 0. 结论速览（对照预注册线）

| 运行 | 覆盖率（n=48 口径） | any-hit | Δ vs off 复跑（0.443） | Δ vs 记录基线（0.448） |
|---|---|---|---|---|
| off 复跑（graph, sample50, judge f1） | 0.443 | 24/48 | — | -0.005（噪声带内） |
| **实验 r1（--llm-rerank 20）** | **0.474** | 25/48 | **+0.031** | +0.026 |
| **实验 r2（--llm-rerank 20，复跑）** | **0.453** | 24/48 | **+0.010** | +0.005 |

- 预注册线：<+0.03 不成立 / ≥+0.05 成立 / 0.03~0.05 灰区。
- **判决：LLM-rerank 路不成立（归档）。** 两次运行 Δ = +0.031 / +0.010，均值
  +0.021 < +0.03；单次最高读数 +0.031 恰好踩灰区下缘，但复跑直接腰斩——
  效应主体是单题（q103）在 N=20 窗口边界的 uid 噪声翻转，不是稳定机制收益。
- 稳定分量只有 q14（0.5→1.0，两次一致，+0.010）；词级语义六轮的量级结论不变：
  句级 LLM 重排的真实收益上限 ≈ +0.01~0.03，**且救回的不是假设中的 A 类/q104 型**。

## 1. 实现设计

通道位置：`answer()` 标准管道证据终排（内容词 BM25，含 think 并入）**截 top-k 之前**
（`bench/src/ingest_answer.cpp`）：

```
recall 大候选池（k_pool=60）+ think 并入
  → 内容词 BM25-lite 终排（stable sort，原机制不动）
  → [--llm-rerank N] 取终排序 top-N（默认实验值 20）：
       每条证据带 dia_id + 首 200 字 → 一次批量 LLM 调用
       （prompt：给定问题与编号证据列表，输出最相关的前 k 个编号，允许"都不相关"）
  → LLM 编号序前 k 置顶进 evidence，其余按 BM25 原序补齐 → 截 top-k
```

- 开关：`--llm-rerank N`（默认 0=关）。默认（0 或 llm_client 空指针）时本段不执行，
  off 路径逐行不变（ctest 20/20 + 单测组4显式验证：有 client 也不被调用）。
- 优雅降级：LLM 空响应（无网/503 重试耗尽）/ 无有效编号（NONE）→ 保持 BM25
  原序，stderr 标注 `[llm-rerank] <qid>: LLM 无有效输出，降级 BM25 原序`。
- 配置：`BenchConfig.llm_rerank` + `BenchConfig.llm_client`（main 注入
  DeepSeekClient；`need_llm` 增加 `llm_rerank>0` 分支；lmem 后端列入
  1.x 专属忽略清单，与 word-sim/final-alpha 同纪律）。
- 作用域：**仅标准管道**。实体档案通道提前返回的题不经过此处（实验中 33/50 题
  因此未触发重排——见 §4 结构性发现，这是本 spike 最重要的负结果之一）。

### Prompt（最终形态）

```
你是检索重排器。给定一个问题（题号 conv-26:q103）和 20 条编号证据
（方括号内为来源标记，文本可能被截断）。
任务：选出对回答该问题最有用的证据，按相关度从高到低给出编号；
若证据都不相关，只输出 NONE。

问题: <question>

E1 [D6:9] <首 200 字>
...
E20 [D7:7] <首 200 字>

只输出编号列表（形如: E3 E1 E7），最多 10 个，按相关度排序。不要输出任何解释。
```

- temperature=0，max_tokens=1024，deepseek-v4-flash（disable_thinking 默认开）。
- **缓存键防污染**：DeepSeekClient 缓存键 = FNV-1a(模型+消息+选项)。prompt 含
  题 id + 逐条 dia_id + 证据文本——不同题、同一题不同证据集天然异键，与
  answer/judge 历史缓存零碰撞。实测 r2 出现 1 次 cache_hit（同题同池序复现），
  无跨题命中。

### 解析容错

`parse_llm_rerank_ids`（导出，单测 8 例）：扫描 `E<num>` token，按出现顺序、
去重、限 1..n_cands、截前 k；词边界保护（`the` 的 `e` 不误配）；NONE / 空响应 /
纯解释文本 → 空 vector = 降级信号。超长的 `E123`（n=20）容错取合法前缀或跳过。

## 2. 判决实验（预注册线，同口径 sample50，graph 后端）

- **API 健康先探测**：单条小请求（"只回答 ok"），HTTP 200，0.9s 返回 `ok`。
  本日内 DeepSeek 间歇 503 未发生；客户端自带 429/5xx 指数退避（5 次），
  失败按降级路径处理（时间盒内未触发）。
- 基线复跑确认：off 复跑 0.443（any-hit 24/48），与记录基线 0.448 的差
  -0.005 在已测定噪声带内（q32 型 uid 噪声翻转，见 lexicon_activation §4.1）。
- 命令（可复跑，`export PATH="$(dirname $(which g++)):$PATH"` 前置）：

```bash
# off
./build-win/bench/ame_bench.exe --dataset locomo --data bench/data/locomo10.json \
  --question-ids bench/data/locomo_sample50.json --extractor rule --answer baseline \
  --judge f1 --k 10 --dump bench/runs/llm_rerank/off_repro.ndjson
# 实验组 = off + --llm-rerank 20（judge 同 f1，零 judge LLM 调用）
./build-win/bench/ame_bench.exe ... 同上 ... --llm-rerank 20 \
  --dump bench/runs/llm_rerank/exp20_r2.ndjson
```

### 成本（api_calls）

| 运行 | api_calls | cache_hits | 说明 |
|---|---|---|---|
| r1 | 17 | 0 | 50 题中仅 17 题到达重排（33 题实体通道提前返回） |
| r2 | 16 | 1 | uid 噪声使池序翻转 → prompt 变化 → 缓存基本不复用 |

单次实验 ≈ 17 次调用 ×（~2.5k in / ~30 out tokens）。对比：judge=llm 一次全量
评测即 50 次调用——重排通道的 API 成本量级与一次 judge 相当，成本本身不是
否决项；**收益才是**。

## 3. 逐题分析（救回的是不是 A 类/q104 型）

两次运行合计救回 2 题次（r1: q103+q14；r2: q14），**0 题是 A 类，0 题是 q104 型**：

### conv-26:q14（cat3，taxonomy C 类，0.5 → 1.0，两次稳定救回）

- 问题：`Would Caroline still want to pursue counseling as a career if she hadn't
  received support growing up?`（反事实改述）
- 金标：D4:15 + D3:5。off 终排：D4:15 在第 7 位（coverage 0.5），D3:5 落在
  top-10 之外。LLM 把 D3:5（BM25 第 16 位）选进前 k → coverage 1.0。
- 机制：反事实条件句与证据的句级语义匹配，词面 BM25 无能力做到——
  这是通道机制正确性的正面证据，但只值 +0.010。

### conv-26:q103（cat4，taxonomy C 类，r1: 0 → 1.0；r2: 0 → 0）

- 问题：`What was Melanie's favorite book from her childhood?`
- 金标 D6:10 `I loved reading "Charlotte's Web" as a kid...`——与问题词面零内容词
  重合（"childhood"↔"as a kid"、"favorite"↔"loved reading"），BM25 把它排在
  10~20 位边界（off dump 中不在 top-10；r1 候选池在 top-20 内 → LLM 选中置顶；
  **r2 候选池翻转到 20 名开外 → 通道根本没看见金标**）。
- 该题是"N=20 窗口边界噪声"的直接实证：同一通道、同一配置，救不救得回看
  uid 噪声抽签。

### 证据链（r1 dump，3 个典型）

```
q14  反事实改述题
  gold: D4:15, D3:5
  off  top-k 前4: D7:7, D4:11, D4:14, D5:3   （D4:15 第 7，D3:5 缺席）
  exp  top-k 前4: D4:15, D7:7, D5:3, D4:11   （D3:5 第 10 —— LLM 从第 16 位捞回）
  exp top1 证据: "Thanks, Melanie. It really mattered. My own journey and the
                 support I got made a huge difference. Now I want to help people..."

q103 童年最爱书改述题
  gold: D6:10
  off  top-k: 无 D6:10（D6:9 "kids' books" 词面命中居首）
  exp  top1 证据: "I loved reading \"Charlotte's Web\" as a kid. It was so cool
                 seeing how friendship and compassion can make a difference."

q18  A 类改述鸿沟（taxonomy 目标题）——通道触达但正确判 NONE
  gold: D4:6, D6:16, D8:32（3 条金标无一进入 top-20 候选）
  LLM 输出 NONE → 降级 BM25 原序 → coverage 维持 0
  （LLM 判断正确：池内确实没有能回答问题的证据）
```

## 4. 结构性发现（比判决数字更重要）

1. **重排只覆盖 34% 的题面**：50 题中 33 题被实体档案通道提前返回（人名题
   基本全被它截走），从未到达重排点。A 类 5 题中 3 题（q3/q105/q156）、
   q104 近失、B 类 q10/q155 都在其中。**假设中的主要救援对象恰恰大多不经过
   本通道**——这不是 N 大小的调参问题，是通道位置问题。
2. **N=20 漏金标实测**：12 个 taxonomy 目标题中 6 个到达重排（A 类 q18/q64，
   B 类 q34/q50/q101/q154；其余 6 个被实体通道截走），**这 6 题金标在
   BM25 top-20 内的为 0/6**（与其 lmem pool_rank 12~58 一致）。LLM 重排是
   排序机制，对召回鸿沟结构性失明——4 题 NONE 降级全部是金标不在池的
   正确判断。
3. 救回的 2 题都是 **C 类（词面重合 ≥0.05、金标在池内 10~20 位）**——即
   LLM 重排的真实能力边界：把"已在池内、BM25 排 11~20 的改述金标"捞回
   top-k。该集合在本样本里只有 ~2 题，收益上限 ≈ +0.01~0.03。

## 5. 判决

**预注册线对照**：Δ = +0.031 / +0.010（vs off 复跑 0.443），均值 +0.021
< +0.03 → **LLM-rerank 路不成立，按纪律归档**。

- 不触发"便宜近似"设计任务（≥+0.05 才触发）；
- 不进 0.03~0.05 灰区复判（均值未达下缘；r1 单读数踩线但 r2 证伪其稳定性）；
- 句级语义收益上限的判决实验闭环：词级 +0.016（词面范式上限）→
  句级重排 +0.01~0.03（且救回对象错位）。**剩余 miss 的主体（A 类召回鸿沟 +
  实体通道遮蔽）靠排序层任何变体都无法解决**，若继续句级方向，唯一没被本
  spike 试过的形态是**句级召回/扩池**（LLM 生成改述查询再召回，改变的是池
  成员而非排序）——但那已经超出"重排"范畴，立项需另起任务书。

## 6. 蓝方自检逐条

1. **LLM 输出解析容错**：✅ 8 例单测（编号序/去重/越界/NONE/空/散文夹带/
   词边界/k 截断）；集成测试覆盖"返回 E2 置顶 / 空响应降级 / 无编号降级"。
2. **证据截断 200 字丢关键信息**：LoCoMo 单轮文本绝大多数 <200 字；截断只
   作用于 prompt 展示，不影响落库证据；且重排只需相关性判断而非精确抽取。
   残留风险：超长轮（含上下文拼接的前/后轮）尾部信息不可见——本样本未观测
   到因此造成的误判（4 题 NONE 均经人工核对为池内无金标）。
3. **N=20 漏金标概率估算**：不用估——实测到达重排的 6/6 taxonomy 目标题
   金标全部在窗口外（B 类 6/6）；q103 证明边界题（金标 rank 10~20）跨运行
   翻转率 ≥50%（r1 救回 / r2 窗口外）。窗口效应主导了判决数字的方差。
4. **LLM 相关≠金标的偏差方向**：两个方向均有理论风险——(a) 置顶词面相近
   非金标（false positive，压低覆盖率）；(b) 漏选需推理链的金标。实测两个
   方向都未造成净伤害：两次运行"丢失"列表均为空（置顶 13 题零回退），
   4 题 NONE 全是金标不在池的正确判断。但注意样本量小（13 置顶），
   该结论的统计功效有限。
5. **缓存污染**：prompt 含题 id + 证据集内容（dia_id+文本），键天然隔离；
   实测无跨题命中（r2 仅 1 次同题同池序自命中）。
6. **uid 噪声 → 缓存不复用**：两次运行仅 1/33 命中——"复跑零成本"在 uid 噪声
   管线上不成立（沿用 lexicon_activation §4.1 的既定纪律）；单题 prompt 成本
   ~2.5k tokens，可接受。
7. **off 行为逐行不变**：`llm_rerank==0` 或 client 空指针时新段不执行；
   ctest 20/20 全绿（9.03s）；单测组4显式断言默认 cfg 下 FakeLlmClient
   零调用；新增诊断日志仅在 `--llm-rerank>0` 时输出。
8. **实体通道遮蔽是测量偏差吗**：33 题未触达重排，其中含 6 个 taxonomy 目标
   ——若重排也挂在实体通道内，收益上限会更高吗？不会：这些题金标同样不在
   实体通道候选池的 top-N 内（B 类 q10/q155 lmem pool_rank 49/58，A 类金标
   不在池），排序机制对召回鸿沟失明的结论与通道位置无关。此判断写入 §4.2。

## 7. 改动清单

- `bench/include/ame/bench/bench.h`：BenchConfig + `llm_rerank`/`llm_client`；
  前置声明 `ILlmClient`；导出 `parse_llm_rerank_ids`（单测用）。
- `bench/src/ingest_answer.cpp`：`parse_llm_rerank_ids`（容错解析）、
  `llm_rerank_select`（prompt 构造 + 单次调用 + 降级）、`answer()` 终排后
  截 top-k 前插入重排段（默认关闭不执行）；`--llm-rerank>0` 时的诊断日志
  （候选/选中 dia_id、实体通道与候选≤1 未触达归因）。
- `bench/cli/main.cpp`：`--llm-rerank N` 解析 + usage；`need_llm` 增分支；
  client 注入 cfg；lmem 忽略清单增 `llm-rerank`。
- `bench/CMakeLists.txt`：ame_bench 链接 ame_llm（PUBLIC）。
- `tests/test_bench.cpp`：`test_llm_rerank_parse`（8 例）+
  `test_llm_rerank_channel`（置顶/降级×2/默认关闭，4 组）。
- 新脚本：`bench/scripts/llm_rerank_compare.py`（A/B 逐题对照 + taxonomy 标签
  + rerank 状态归因）。
- ctest **20/20 全绿**（9.03s，含新增用例）。
- 运行产物（gitignored）：`bench/runs/llm_rerank/{off_repro,exp20,exp20_r2}.*`、
  `bench/cache/`（33 个响应缓存）。
