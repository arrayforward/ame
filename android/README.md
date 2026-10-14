# AME Android 对话 App

封装 AME 联想记忆引擎做**上下文外置/长期记忆**的对话 App，模型走 MiniMax
（OpenAI 兼容 API）。每个 session 独立记忆、独立上下文，滑动窗口之外的历史
只存在于 AME 记忆图中（"外置而非压缩"——不摘要、不丢弃，靠 recall 取证回填）。

## 架构

```
┌─ App（Java）────────────────────────────────┐
│  MainActivity（session 列表：新建/进入/删除）  │
│  ChatActivity（消息列表 + 输入）               │
│  MemoryOrchestrator ── 每轮流水线：            │
│    ① remember_turn(user)                       │
│    ② 装配上下文 = 档案槽位 + recall 证据        │
│       （带时间戳） + 最近 8 条原文滑窗（4 轮）         │
│    ③ MiniMaxClient 整段作答（OkHttp）          │
│  ④ remember_turn(assistant) + jsonl + 快照    │
└──────────────┬─────────────────────────────────┘
               │ JNI（libame_jni.so，句柄表按 session 管理 Engine）
┌──────────────▼─────────────────────────────────┐
│  AME 引擎（C++17，仓库源码零分叉复用）          │
│  core/storage/time/vector/extractor/entity/    │
│  keyword/graph_writer/seed/diffusion/rerank/   │
│  thinker/feedback/learning/engine              │
│  （llm/bench 不进 Android；联网在 Java 侧）   │
└────────────────────────────────────────────────┘
```

## JNI 接口（`ame_jni/ame_jni.cpp` ↔ `AmeEngine.kt`）

| 方法 | 语义 |
|---|---|
| `nativeCreateSession(dir) → jlong` | 建 Engine 并加载 dir/graph.json 快照（若有），返回句柄 |
| `nativeDestroySession(handle, dir)` | 快照兜底落盘后销毁实例 |
| `nativeRememberTurn(handle, speaker, text, ts, diaId, sessionId) → int` | 写一轮对话（user/assistant 双侧） |
| `nativeRecall(handle, query, limit) → String` | 记忆检索，返回 JSON `[{text,ts,score}]` |
| `nativeGetProfile(handle) → String` | 用户代理实体档案槽位 JSON `{name,slots:[...]}` |
| `nativeSave/nativeLoad(handle, dir) → int` | 显式快照落盘/恢复 |

持久化布局：`filesDir/sessions.json`（会话元数据）、
`filesDir/sessions/<id>/messages.jsonl`（消息流）、`graph.json`（AME 图快照）。

## 构建

- **Android Studio**：直接导入 `android/` 目录，Sync 后 Run。
- **命令行**：

```bash
cd android
gradle assembleDebug   # 产出 app/build/outputs/apk/debug/app-debug.apk
```

前置：Android SDK（local.properties 的 `sdk.dir`）、NDK 27（build.gradle 已钉
`ndkVersion '27.0.12077973'`）、JDK 17+。ABI 只打 arm64-v8a（minSdk 26 / target 34）。

## MiniMax 配置（key 绝不入库）

`android/local.properties`（已 gitignore）：

```properties
sdk.dir=<Android SDK 路径>
MINIMAX_API_KEY=<你的 key>
MINIMAX_BASE_URL=https://api.minimaxi.com/v1
MINIMAX_MODEL=MiniMax-M3
CONTEXT_WINDOW_TURNS=8       # 可选：滑动窗口消息条数（8 条=4 轮，状态栏同步显示）
RECALL_TOP_K=5               # 可选：recall 证据条数（状态栏同步显示）
```

代码经 `BuildConfig` 读取，日志/提交不出现 key。

## 模型口径（MiniMax-M3 实测记录）

- **上下文**：M3 为 256k 上下文模型；`max_tokens` **含思考预算**——响应 usage 里
  有 `completion_tokens_details.reasoning_tokens`，思考会吃掉输出额度，App 默认
  `max_tokens=2048` 防截断。
- **推理段内联**：content 里带 `<think>...</think>`，App 端 `stripThink()` 剥离后
  才展示/入库。
- **多模态实测**（curl，2026-10-13）：OpenAI 兼容 `content` 数组 + `image_url`
  data URI（`data:image/jpeg;base64,...`）HTTP 200 可用——512×256 黑字白底
  "HELLO AME" 测试图，模型正确读出文字。请求样例（截断）：

```json
{"model":"MiniMax-M3","max_tokens":512,"messages":[{"role":"user","content":[
  {"type":"text","text":"图片里写了什么文字？"},
  {"type":"image_url","image_url":{"url":"data:image/jpeg;base64,/9j/..."}}]}]}
```


## 记忆工作方式

- **写入**：每轮 user/assistant 双侧 `remember_turn`（实体解析/关键词/共现边/
  NEXT 链由引擎自动完成），轮后 `save()` 快照。
- **读取**：用户档案槽位（居住地/职业/偏好等，双时态当前值）+ 对当前输入做
  recall 取 top-5 证据（带 `[yyyy-MM-dd]` 时间戳，供模型时序推断）。
- **滑窗**：最近 8 条（4 轮）原文直进上下文；更早的历史不进 prompt，只在 AME 图中，
  由 recall 按需取证——上下文长度恒定，记忆容量无界。
- **session 隔离**：每个 session 独立 Engine 实例与快照文件；删除 session 时
  目录（消息+快照）递归清除。

## 状态栏（上下文外置可视化）

聊天页顶部实时对比行：`记忆 N轮 ~Xtok │ 发送 Ytok │ 压缩 Z× │ 节点 M │ 窗口8+证据5`。

- **累计记忆**：引擎内总轮数（`Engine::stats().turns`）与 token 估算
  （`total_chars/2`，中英混合均值口径）——随对话无限增长。
- **实际发送**：本轮发给模型的 messages token 估算（CJK 1 字≈1 token、
  ASCII 4 字符≈1 token、图片 1500 token/张——最长边≤1024 JPEG 的经验口径）——
  滑窗+证据装配下恒定有界。
- **压缩比** = 累计/发送；节点数展示记忆图规模（事件+实体+关键词）。
- 数据通路：JNI `nativeStats()` → `{turns,total_chars,events,nodes}`；
  发送侧估算在 Java 装配时累计。

## 联网搜索（输入栏「联网」开关）

- 流程（两轮）：第 1 轮 `web_search` 只查内容（Responses API，单次请求内搜索+整理，
  只发"搜索人设+查询"，不带对话历史——历史里的陈旧结论会带偏模型放弃搜索）；
  第 2 轮 M3 本地非联网作答，system prompt 同时携带**联网搜索结果 + recall 记忆
  证据 + 提问**。
- 记忆口径：**第 1 轮检索内容不落记忆**（时效内容入库会污染召回）；只有第 2 轮
  的整轮问答按惯例写成"提问 → 回答"一条记忆。
- 带图轮：先用 M3 vision 把图片解析成文本（详细描述关键信息）并入搜索查询。
- 搜索失败自动降级为不联网直接作答；诊断 tab 展示第 1 轮检索原文与注入位置。
- 实测备注：`/v1/chat/completions` 带 web_search 工具返回 400；Responses 接口
  顶层 `output_text` 为检索结果。

## 多模态输入

- 入口：聊天页「选图」（Photo Picker，免权限）/「拍照」（系统相机 + FileProvider，
  不声明 CAMERA 权限）。
- 压缩：最长边 ≤1024、JPEG 80 → base64 data URI 发送（OpenAI 兼容格式）。
- 记忆入库：图片轮存「[图片] + 随图文字」；无随图文字时调一次 M3 vision 生成
  一句话描述（失败存占位文本）——图片语义由此可被 recall 检索；图片本体不进
  记忆图，recall 证据只回传文本。消息列表显示缩略图（imagePath 落 jsonl，
  重载可还原）。

## 导游模式（底部「导游」tab）

文旅导览场景：**一个游客一个 session**，每个导游 session 绑定一份导览方案
PDF；游客拍照 + 提问，按方案回答，同时保留该游客在本次导览中的对话记忆；
删除 session 即清除该游客的全部记忆（目录递归删除，含方案与图快照）。

- **方案导入**：`MainActivity` 导游 tab 选 PDF（GetContent 免权限）→ 复制到
  `sessions/<id>/guide.pdf` → `GuidePdf`（pdfbox-android，纯 JVM 不联网）抽全文
  文本 + 递归抽内嵌图（XObject 表单），无内嵌图时用系统 PdfRenderer 渲染页图兜底；
  文本按 200 字/块、上限 300 块 `rememberTurn("guide", …)` 灌入记忆图（speaker=
  guide），快照落盘。
- **问答**：`GuideActivity` 与 ChatActivity 同构，多一个「方案」按钮（弹窗看
  方案全文 + 图片）。`MemoryOrchestrator` 传 `guideTitle/guideText` 即切导游人设：
  system prompt = 导游人设 + 导览方案前 8000 字（`PROMPT_CHARS`，超出截断并
  提示——截断外细节靠 recall 命中已灌入的方案块）+ 游客档案 + recall 证据。
- **持久化布局**（session 目录内增量）：
  `guide.pdf`（原件）、`guide.txt`（全文）、`guide_img/`（内嵌图）、
  `guide_page/`（页渲染图）、`graph.json`/`messages.jsonl`（原有）。
- **session 类型隔离**：`sessions.json` 元数据加 `type`（chat/guide，旧数据默认
  chat）与 `pdf` 字段；两个 tab 各自过滤，列表互不串、记忆互不串。
