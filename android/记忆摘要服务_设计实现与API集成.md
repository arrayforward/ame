# 记忆摘要服务：设计实现 · API · 集成文档

> 版本：dev-4.0（Android demo 应用）｜ 代码：`app/src/main/java/com/ame/memory/MemorySummaryService.java`
> 依赖：`MiniMaxClient`（LLM，MiniMax-M3）、`AmeEngine`（AME 图引擎 JNI）、org.json。

## 一、设计

### 1.1 要解决的问题

Android demo 里引入 LLM 做上下文整理、用 AME 做知识库检索时，**一次性取回的数据太复杂**：
原始知识文本/检索结果/对话上下文直接塞给模型，既浪费 token 又稀释重点。
解法是把"记忆整理"独立成 LLM 摘要过程：先整理成**结构化、分层、可按需取用**的摘要，
原始文本仍然可溯，但注入多少由调用方决定。

### 1.2 三层数据模型（+联想词）

| 层 | 内容 | 用途 | 是否进上下文 |
|---|---|---|---|
| L1 基础摘要 | 几十字（≤60 字） | 言简意赅回答当前问题 | 默认注入 |
| L2 详细整理 | ≤300 字要点/因果/时序 + **联想词** | 难理解问题的参考；联想词供 Java 做联想检索 | 可选注入 |
| L3 原始文本 | 完整原始知识 | 细节追溯、逐字证据 | 可选注入 |

统一 JSON 载体（生成、存储、检索共用同一格式）：

```json
{
  "l1": "公司允许远程办公",
  "l2": "2024 年起全员可申请远程；需直属主管审批；每周到岗不少于 1 天……",
  "keywords": ["远程办公", "考勤制度", "主管审批"],
  "l3": "（完整原始知识文本）"
}
```

### 1.3 数据流

```
注入知识 / 检索知识 / 对话上下文
        │  ① summarize(source, question)   LLM 记忆整理（严格 JSON，失败降级截断）
        ▼
  Summary（L1/L2/keywords/L3）
        │  ② storeToAme(s, ts)             完整 JSON + "MEMSUM:" 前缀，单事件写入
        ▼
   AME 记忆图（session 隔离；走 cheap/深度重排同一检索管线）
        │  ③ fetchFromAme(query, limit)    recall → 过滤 MEMSUM → 还原完整 JSON
        │  ④ fetchByKeywords(kws, n)       L2 联想词逐个 recall，去重合并（联想检索）
        ▼
        ⑤ buildContext(items, level)       level 1/2/3 分层装配（0=不注入）
```

### 1.4 关键决策

- **完整 JSON 直接作事件文本**：不改动 C++ 图结构，复用现有 recall/重排/快照；
  `MEMSUM:` 前缀使摘要事件与普通对话事件可区分（检索时过滤，普通对话事件忽略）。
- **L3 永取调用方原文**：不让模型复述原文，避免转述失真；模型只产 L1/L2/联想词。
- **失败降级不阻断**：LLM 或 JSON 解析失败时退回截断摘要（L1=前 60 字、L2=前 300 字），
  与全 App"降级优于崩溃"的既有约定一致；降级有 logcat 记录。
- **session 隔离不变**：服务持有 `AmeEngine + sessionId`，写入即该 session 的图，
  删 session 即清（与"每 session 独立记忆"语义一致；租户共享走 dev-4.0 的 attach_tenant）。
- **层级注入是调用方自由**：`buildContext` level=0/1/2/3 对应"不提供 / 仅 L1 / L1+L2 / 全量"，
  满足不同成本与难度场景（便宜问答只给 L1，难题才展开 L2/L3）。

## 二、实现

单文件 `MemorySummaryService.java`（约 200 行，无新增依赖）：

- `Summary`：三级+联想词的 POD，`toJson()/fromJson()` 即统一载体；字段缺失按空容错。
- `summarize`：system 限定"只输出严格 JSON"，user=当前问题(可空)+材料；
  响应剥离代码围栏并取首 `{` 末 `}` 区间解析；`parsed.l3 = source` 回填原文。
- `storeToAme`：`rememberTurn("summary", MARK + s.toJson(), ts, "sum"+ts, sessionId)`，
  返回 `rc==0`；日志带三层字数与联想词数。
- `fetchFromAme`：`engine.recall` 返回 `[{text,ts,score}]`，过滤 `MEMSUM:` 前缀后
  `Summary.fromJson`；单条解析失败只跳过该条。
- `fetchByKeywords`：联想词逐个 `fetchFromAme`，`LinkedHashSet` 按完整 JSON 去重。
- `buildContext`：按 level 输出【摘要】/【详述】/【联想词】/【原文】分段文本。

线程约定：全部同步阻塞，调用方在 io 线程使用（同 `MemoryOrchestrator`）。

## 三、API 文档

构造：

```java
MemorySummaryService svc = new MemorySummaryService(engine, sessionId);
```

| 接口 | 签名 | 说明 |
|---|---|---|
| 生成摘要 | `Summary summarize(String source, String question)` | LLM 整理材料为三级 JSON+联想词；`question` 可空；失败降级截断摘要 |
| 写入 AME | `boolean storeToAme(Summary s, long ts)` | 完整 JSON 作为 MEMSUM 事件写入本 session 图；返回是否成功 |
| 检索还原 | `List<Summary> fetchFromAme(String query, int limit)` | recall 后还原完整 JSON（三级+联想词），非摘要事件自动忽略 |
| 联想检索 | `List<Summary> fetchByKeywords(List<String> keywords, int limitPerKeyword)` | 联想词逐个 recall，去重合并 |
| 分层装配 | `String buildContext(List<Summary> items, int level)` | 0=空串；1=仅 L1；2=L1+L2+联想词；3=含 L3 全量 |

`Summary` 字段：`String l1 / l2 / l3`，`List<String> keywords`；
`toJson()` / `Summary.fromJson(json)` 为统一序列化对。常量 `MemorySummaryService.MARK = "MEMSUM:"`。

## 四、集成指南

### 4.1 写入侧：知识入库先整理

```java
MemorySummaryService svc = new MemorySummaryService(engine, sessionId);
Summary s = svc.summarize(rawKnowledge, /*当前问题，可空*/ null);
svc.storeToAme(s, System.currentTimeMillis() / 1000);
```

对话上下文整理同理：把滑窗外的旧对话拼成 source 调 `summarize`，
摘要入图后，后续问答只需 recall 摘要层，不必回贴原文。

### 4.2 读取侧：按问题难度选注入层级

```java
List<Summary> items = svc.fetchFromAme(question, 5);
String context = svc.buildContext(items, 1);   // 常规：只给 L1，最省 token
// 难题：buildContext(items, 2)；要逐字证据：buildContext(items, 3)
// system prompt = 人设 + context + 既有 recall 证据 + 滑窗
```

### 4.3 联想检索（L2 联想词的用途）

```java
List<Summary> first = svc.fetchFromAme(question, 5);
LinkedHashSet<String> kws = new LinkedHashSet<>();
for (Summary s : first) kws.addAll(s.keywords);
List<Summary> more = svc.fetchByKeywords(new ArrayList<>(kws), 3);  // 联想扩展
String context = svc.buildContext(more, 1);
```

### 4.4 与现有三档重排的关系

摘要事件走与普通事件**完全相同的** recall 管线（cheap 默认档池 60→topN，
深度档 M3 全池打分），无需额外开关；摘要体积小，通常还能降低注入 token。

### 4.5 注意

- `buildContext(items, 0)` 或 items 为空返回空串——"不提供到上下文"就是 level 0。
- LLM 摘要本身有成本（一次 chat/条），写入侧批量整理时建议复用 io 线程并控制并发。
- 摘要事件 `speaker="summary"`，诊断 tab 中可与 user/assistant 轮次区分。

## 五、验证

- `assembleDebug` 编译打包通过（无新增依赖，org.json 为平台自带）。
- 真机自验路径：logcat `摘要入图 rc=0` → 提问相关知识 → `摘要检索 '...' -> N 条`；
  诊断 tab 可见 MEMSUM 事件与分层注入文本。
- 本工程无 Java 单测基建（同既有约定，编译+符号+真机三级验证）。
