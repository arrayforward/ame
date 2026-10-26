# AME 引擎 Java/Android 集成文档

> 面向"把 AME 记忆引擎嵌入 Android App"的集成指南：环境、构建、运行时架构，
> 以及每个设计决策背后的原理与手机端实证教训。前置阅读：`../README.md`
> （引擎总览）、`../docs/design/AME2.0_分层记忆架构设计.md`（分层设计）。
> 当前版本对应 dev-2.0 分支 `2e96e84`。

---

## 1. 总览

```
App（Java，纯 Java 无 Kotlin）                    AME 引擎（C++17，零第三方依赖）
┌────────────────────────────────────────┐      ┌────────────────────────────────┐
│ MainActivity（session 列表+双 tab）      │      │ core / storage / time / vector │
│ ChatActivity / GuideActivity（聊天页）   │ JNI  │ keyword / extractor / entity   │
│ MemoryOrchestrator（记忆编排，核心逻辑）  │ ───► │ graph_writer / seed / diffusion│
│ MiniMaxClient（LLM/OpenAI 兼容 API）     │ 桥   │ rerank / engine（12 模块）    │
│ SessionStore / KeywordDict / CheapRerank │      │ （3.0 单后端 graph；2.0 分层  │
│ GuidePdf / AmeEngine（JNI 薄壳）         │      │  存储已裁，留存 dev-2.0）     │
└────────────────────────────────────────┘      └────────────────────────────────┘
        联网：MiniMax M3（对话/vision/搜索）+ web_search Server Tool
```

设计取向一句话：**引擎零依赖、零分叉**（仓库 24 个 C++ 模块源码直接编进
`libame_jni.so`），联网全部在 Java 侧（OkHttp），业务逻辑（orchestrator/
client/store）是**纯 Java + OkHttp + org.json，不依赖 Android UI**——为移植
Spring AI 预留的边界。

## 2. 环境准备

| 依赖 | 版本 | 说明 |
|---|---|---|
| JDK | 17+ | `compileOptions` 已钉 17 |
| Android SDK | compileSdk/targetSdk 34, minSdk 26 | `local.properties` 的 `sdk.dir` |
| NDK | **27.0.12077973** | `build.gradle` 钉死，版本不符会 CMake 报错 |
| CMake | 3.22.1 | AGP `externalNativeBuild` 自动带 |
| Gradle | 8.13（本机）/ AGP 8.7.3 | 无 wrapper 时用本机 `~/.gradle/wrapper/dists/` 下的发行版 |

`android/local.properties`（gitignore，不入库）全配置项：

```properties
sdk.dir=C:\\Users\\<你>\\AppData\\Local\\Android\\Sdk
MINIMAX_API_KEY=<key>            # 必填，Bearer 认证
MINIMAX_BASE_URL=https://api.minimaxi.com/v1   # 可选，默认官方
MINIMAX_MODEL=MiniMax-M3         # 可选，默认 M3（256k 推理模型）
CONTEXT_WINDOW_TURNS=8           # 可选，滑窗消息条数（=4 轮对话）
RECALL_TOP_K=5                   # 可选，默认档证据条数（cheap 重排后注入条数）
```

命令行构建（Windows，Git Bash）：

```bash
cd android
"$HOME/.gradle/wrapper/dists/gradle-8.13-bin"/*/gradle-8.13/bin/gradle.bat \
    :app:assembleDebug          # → app/build/outputs/apk/debug/app-debug.apk
```

## 3. 构建原理（为什么这样搭）

- **C++ 侧**：`app/build.gradle` 的 `externalNativeBuild` 指向
  `ame_jni/CMakeLists.txt`，后者 `add_subdirectory` **仓库根 CMakeLists**——
  3.0 的 12 个 M 模块（M1–M10/M12/M13，M8/M11/M14 与 2.0 M15~M19 已裁）
  源码**零分叉**直接编进 `libame_jni.so`；bench/llm/tests/demo 不进 Android。
  ABI 只打 `arm64-v8a`（体积与真机覆盖的取舍）。
- **JNI 单桥**：`ame_jni.cpp`（Engine 外观 + cheap 重排模型注入），共用
  `jni_util.hpp`（MUTF-8↔UTF-8 双向转换、全局句柄表）。句柄用单调递增
  `jlong`（防悬空复用），close 先摘表再等锁，destroy 走"快照兜底落盘"。
- **词典进 APK**：Gradle `copyKeywordDict` 任务构建期从仓库 `lexicon/` 拷贝
  67.8 万词词表进 assets（不进源码树）；首启拷到 `files/dict/` 并建 AC 二进制
  缓存，**全 App 一份、多 session 共享秒载**（`KeywordDict.ensureLoaded`）。
- **cheap 模型进 APK**：`assets/cheap_model.json`（668B，11 特征+权重+bias）
  随源码入库；建 session 时 `CheapRerank.ensureLoaded` 注入 JNI，默认启用。
- **key 安全**：只经 local.properties→BuildConfig，日志/仓库不落；debug 签名。

## 4. 运行时架构

- **生命周期**：每个 session 一个引擎实例，随 Activity 创建/销毁；
  `onDestroy` → `close()` → 快照兜底。**无 Service 常驻**（简单优先，
  代价是离开页面即释放、回页重载——研究期取舍，常驻化在路线图）。
- **异步初始化**：引擎创建（载 graph.json 快照）+ 词典/cheap 模型加载在 io 线程，
  `ready` 门控——**历史上"进 session 卡死"就是 UI 线程同步载快照所致**；
  网络回调晚于销毁会碰已 shutdown 的线程池，`refreshStatus` 有
  `io.isShutdown() || !ready` 双守卫（`RejectedExecutionException` 崩溃的教训）。
- **线程模型**：单线程 `ExecutorService` 串行全部记忆操作（天然免锁）；
  OkHttp 同步阻塞（readTimeout 180s 容忍 M3 长思考），UI 只刷新列表；
  非流式作答配"思考中…"占位消息。
- **图片不落盘**：聊天图片只在内存压成 base64 发送，记忆只存 M3 vision 生成
  的一句话描述文本（≤40 字）；相机合约必须有落点 → cacheDir 临时文件用完即删
  （`file_paths.xml` 记得配 `cache-path`——漏配会点拍照即崩）。

## 5. 存储后端（3.0：单后端 graph）

每 session 一张完整记忆图（事件+实体+关键词同图），整图 JSON 快照节流落盘
（每 10 轮 + 销毁兜底——每轮全量 dump 的 O(N²) 卡顿教训）。
2.0 分层存储（lmem/MemEngine 双后端、观测层分片与自动迁移）已在 3.0 裁撤
（研究期评测全链未超 1.x，证据见 `../docs/reports/lmem_p0_report.md` 等），
实现留存 dev-2.0 分支；`MEM_STORE` 配置项已移除。

## 6. 记忆设计原理（手机端实证版）

| 设计 | 原理 | 实证教训 |
|---|---|---|
| **外置而非压缩** | 滑窗外历史不进 prompt、不摘要，只活于记忆图，recall 按需取证 | 状态栏"压缩比"=累计/发送；上下文恒定有界 |
| **一轮一条** | "提问 → 回答"合成单事件一次写入；提问永不单独成图 | 根治"X 你还记得吗"模板问题互相命中挤占召回（welink 事件） |
| **先取证、后写入** | 装配（recall）在用户轮入图**之前**执行 | 当前问题不以满分自命中占证据位 |
| **词典必备** | M1 无词典时 CJK 词法通道全灭，只剩字符二元组兜底 | "召回不了记忆"排查的根因之首 |
| **MUTF-8 双向** | `NewStringUTF` 要修改版 UTF-8，emoji 直接传会乱码 | 证据/system prompt 里的 🎒 变 � 的 bug |
| **搜索结果不落记忆** | 时效内容入库会污染召回（陈旧结论当事实） | "我没有联网"污染搜索的教训 |
| **assistant 观测低可靠** | 助手回答是解释不是事实，证据排序降权 | 陈旧结论污染（上一条的另一面） |

token 估算口径（仅展示用）：CJK 1 字≈1 tok、ASCII 4 字符≈1 tok、
图 1500 tok/张、累计记忆 = 引擎字符数 ÷2。

## 7. 召回与重排：cheap 默认档与深度档

- **默认档 = cheap 特征重排（3.0 起默认启用）**：建 session 时 `CheapRerank.ensureLoaded`
  把 `assets/cheap_model.json`（bench cheap_model_v2 同模型，11 特征逻辑回归）注入 JNI；
  recall 在原生侧走 **候选池 60 → cheap 特征重排 → top-5**（`ame_jni.cpp` `cheap_rerank`，
  与 bench `--rerank cheap` 同口径：BM25 分/排名、fusion/graph/ctx/time、recency、
  实体重合等题内归一 + 逻辑回归）。**零 LLM、纯 CPU、毫秒级**（PC 实测相对 bm25
  +1ms/题量级）；模型加载失败自动回退引擎原序。bench 同档覆盖率 0.557（sample50）。
- **深度档**（手动开关，蓝底按钮）：召回池扩到 60 → M3 一次批量打分选 top-5
  （`MiniMaxClient.rerank`）。bench 覆盖率 0.672 / judge 0.73。
  **为什么默认关**：七条非-LLM 路线（词级六轮/特征学习/七关系公式/双编码器
  向量/WMD 词组合/cross-encoder 精排/写时增强）全部被数据证伪——句级
  "事实对应"判断在对话改述上只能买 LLM 或大编码器，这是手动开关的商业逻辑，
  证据链在 `../docs/reports/`（lexicon_activation/sentence_semantic_ceiling/
  svec_eval/wmd_spike/xenc_spike/formula_rerank）。
- **作答三指令**（默认生效，bench judge 0.63→0.73 的修复）：时序锚定
  （证据带日期前缀，相对时间必须换算绝对日期）、列表穷尽（复数问题逐项列全）、
  前提归属核对（错误前提先纠正再作答）。在 `buildSystemPrompt` 尾部。

## 8. 联网搜索（两轮流程）

开「联网」后：第 1 轮 `web_search`（MiniMax Server Tool，Responses API，
prompt 限 100 字简洁作答）**只查内容、不落记忆**；检索结果注入本轮 system
prompt 的"联网搜索结果"段；第 2 轮 M3 本地综合"搜索结果+提问+记忆证据"作答，
整轮照常写记忆。带图时先用 M3 vision 解析图片（80 字详细描述）并入查询。

## 9. 导游模式

底部「导游」tab：一个游客 = 一个 session（独立记忆，删即清除）。导入
导览方案 PDF（pdfbox-android 纯 JVM 解析）：全文 → `guide.txt`（system
prompt 注入前 8000 字，截断外细节靠 recall 命中已灌入的方案块）+ 内嵌图/页
渲染图存 `guide_img|page/`；页内「方案」按钮随时查看。记忆机制与普通对话
完全一致（一轮一条）。

## 10. 诊断与调试

- **logcat**：`adb logcat -s AmeMem`——每轮全链路：记忆文本、装配统计、
  逐条证据（score/时间/摘要）、recall 命中数、MiniMax usage、rememberTurn
  返回码、迁移/词典事件。
- **诊断 tab**（聊天页「诊断」按钮）：每轮提交的上下文快照——概况（查询词/
  token/证据数/联网/重排状态）、证据逐条、System Prompt 全文、Messages 逐条
  角色卡片。召回问题先看这里，比 logcat 直观。

## 11. 调参速查

| 旋钮 | 位置 | 默认 | 说明 |
|---|---|---|---|
| 滑窗/证据数 | local.properties | 8 / 5 | 状态栏同步显示 |
| 默认重排 | assets/cheap_model.json | cheap 开 | 池60→11特征重排→topN；删模型即回退引擎原序 |
| 深度档 | UI 手动 | 关 | 每轮 +1 次 LLM 调用 |
| 联网 | UI 手动 | 关 | 每轮 +1 次搜索调用 |
| 导游方案截断 | MemoryOrchestrator.PROMPT_CHARS | 8000 | 方案注入上限 |
| 诊断保留轮数 | DIAG_MAX | 30 | 内存环形 |
