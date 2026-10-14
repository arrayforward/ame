/*
 * 所属模块：app——记忆编排器（上下文外置的核心逻辑）。
 * 主体思路（每轮问答流水线）：
 *   步骤A：装配上下文 = 用户档案槽位（getProfile）+ AME recall 证据（带时间戳，
 *     滑窗外的历史只从记忆图取证，不做压缩摘要——"外置而非压缩"模式）
 *     + 最近 z 条原文（滑动窗口，窗口内图片轮只带其记忆文本）——先于写入执行，
 *     当前问题不入图、不占证据位；
 *   步骤B：MiniMax 整段作答（本轮带图则走多模态 content 数组）；
 *   步骤C：一轮一条写入记忆——"提问 → 回答"合成单个事件一次 remember_turn
 *     （图片轮的记忆文本 = "[图片] " + 随图文字，无随图文字时调 M3 vision 生成
 *     一句话描述，失败存占位文本——让图片语义可被检索，图片本体不进记忆图），
 *     提问永远不单独成图，避免模板问题互相命中挤占召回名额；随后消息落 jsonl
 *     + 引擎快照落盘。失败轮不落记忆（与 jsonl 一致）。
 * 导游模式（guideTitle/guideText 非空）：人设切换为导游助手，导览方案文本注入
 *   system prompt（超 PROMPT_CHARS 截断，截断外细节靠导入时灌入记忆图的方案块
 *   经 recall 取证）；记忆机制与普通对话完全一致（每 session 独立、删即清）。
 * 关键数据结构：token 估算口径——CJK 字符 1 字≈1 token、ASCII 4 字符≈1 token、
 *   图片按定值 1500 token/张（最长边≤1024 JPEG 的经验口径，注释于估算函数）。
 * 依赖关系：AmeEngine（JNI）、MiniMaxClient、SessionStore、BuildConfig（可调上限）。
 */
package com.ame.memory;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

public class MemoryOrchestrator {

    public static final String TAG = "AmeMem";

    /** 图片 token 估算定值（最长边≤1024 的 JPEG，经验口径 ~1500 token/张） */
    public static final int IMAGE_TOKENS = 1500;

    /** 导览方案注入 system prompt 的字符上限（M3 256k 上下文下的保守口径） */
    public static final int PROMPT_CHARS = 8000;

    /** 诊断 tab 保留的最近轮数 */
    public static final int DIAG_MAX = 30;

    /** 联网搜索结果注入字符上限（截断防超长结果撑爆单轮上下文） */
    public static final int SEARCH_MAX_CHARS = 2000;

    private final AmeEngine engine;
    private final SessionStore store;
    private final String sessionId;
    private final int windowSize;    // 滑窗轮数（可调）
    private final int recallLimit;   // 证据条数（可调）
    // 导游模式：非空时切换导游人设，并把导览方案文本注入 system prompt
    // （超出 PROMPT_CHARS 截断；截断外的细节靠导入时灌入记忆图的方案块 recall 取证）
    private final String guideTitle;
    private final String guideText;

    private final SimpleDateFormat fmt =
            new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US);

    /** 上一轮实际发送的上下文 token 估算（状态栏显示用） */
    private int lastSentTokens;

    /** 每轮提交给 LLM 的完整上下文快照（诊断 tab 数据源，最多留 DIAG_MAX 轮） */
    public static final class TurnDiag {
        public final String query;                    // 触发本轮的 recall 查询
        public final String system;                   // system prompt 全文
        public final List<String> evidence;           // recall 证据（带时间戳，与注入一致）
        public final List<MiniMaxClient.Msg> messages;  // 实际提交的 messages（含滑窗）
        public final boolean webSearch;               // 本轮是否开联网
        public final String searchText;               // 第 1 轮联网检索内容；null = 未联网/失败
        public final int sentTokens;
        public final long ts;

        TurnDiag(String query, String system, List<String> evidence,
                 List<MiniMaxClient.Msg> messages, boolean webSearch, String searchText,
                 int sentTokens, long ts) {
            this.query = query;
            this.system = system;
            this.evidence = evidence;
            this.messages = messages;
            this.webSearch = webSearch;
            this.searchText = searchText;
            this.sentTokens = sentTokens;
            this.ts = ts;
        }
    }

    private final ArrayDeque<TurnDiag> diagLog = new ArrayDeque<>();

    /** 短构造：普通对话模式（窗口/证据数取 BuildConfig 默认值，无导游参数） */
    public MemoryOrchestrator(AmeEngine engine, SessionStore store, String sessionId) {
        this(engine, store, sessionId,
                BuildConfig.CONTEXT_WINDOW_TURNS, BuildConfig.RECALL_TOP_K,
                null, null);
    }

    /** 全参构造（Kotlin 默认参数的等价展开：短构造 + 本全参构造） */
    public MemoryOrchestrator(AmeEngine engine, SessionStore store, String sessionId,
                              int windowSize, int recallLimit,
                              String guideTitle, String guideText) {
        this.engine = engine;
        this.store = store;
        this.sessionId = sessionId;
        this.windowSize = windowSize;
        this.recallLimit = recallLimit;
        this.guideTitle = guideTitle;
        this.guideText = guideText;
    }

    public int getLastSentTokens() {
        return lastSentTokens;
    }

    public List<TurnDiag> diag() {
        return new ArrayList<>(diagLog);   // toList()：快照副本
    }

    /** 引擎统计（JNI 透传）：{turns,total_chars,events,nodes} */
    public String statsJson() {
        return engine.stats();
    }

    /** 一轮问答（纯文本）；返回助手回复文本；全程同步，调用方负责切线程 */
    public String chat(String userText) {
        return chat(userText, null, null, false);
    }

    /** 一轮问答；返回助手回复文本；全程同步，调用方负责切线程。
     *  记忆写入模式：一轮一条——"提问 → 回答"合成单个事件一次写入（rememberTurn
     *  每轮只调一次），提问永远不单独成图，避免模板问题互相命中挤占召回名额；
     *  写入在作答成功后进行（失败轮不落记忆，与 jsonl 一致）。
     *  webSearch=true 时两轮流程：第一轮 web_search 只查内容（不落记忆），第二轮
     *  M3 本地非联网作答——system prompt 同时携带"联网搜索结果 + recall 记忆证据 +
     *  提问"；只有第二轮的整轮问答写入记忆（一轮一条，失败轮不落）。带图时先用
     *  M3 vision 解析图片并入搜索查询。搜索失败降级为不联网直接作答。 */
    public String chat(String userText, String imageB64, String imagePath,
                       boolean webSearch) {
        long now = System.currentTimeMillis() / 1000;
        List<Message> history = store.loadMessages(sessionId);
        Log.d(TAG, "[" + sessionId + "] chat 开始: user=" + take(userText, 80)
                + " 带图=" + (imagePath != null) + " 联网=" + webSearch
                + " history=" + history.size() + "条");

        // 图片轮的记忆文本 = "[图片] " + 随图文字，无随图文字时调 M3 vision 生成
        // 一句话描述（失败存占位文本）——让图片语义可被检索（图片本体不进记忆图）
        String memoryText;
        if (imageB64 == null) {
            memoryText = userText;
        } else if (!userText.trim().isEmpty()) {   // isNotBlank
            memoryText = "[图片] " + userText;
        } else {
            String cap = MiniMaxClient.visionCaption(imageB64);
            memoryText = "[图片] " + (cap.isEmpty() ? "用户分享了一张图片" : cap);  // ifBlank
        }
        Log.d(TAG, "[" + sessionId + "] 记忆文本: " + take(memoryText, 120));

        // 联网流程第 1 轮：只查内容，不写入记忆（搜索结果时效性强，入库会污染召回）
        String searchText = null;
        if (webSearch) {
            try {
                String q = userText.trim().isEmpty() ? memoryText : userText;
                if (imageB64 != null) {
                    String desc = MiniMaxClient.visionForSearch(imageB64);
                    if (!desc.isEmpty()) {
                        q = userText.trim().isEmpty()
                                ? "图片内容：" + desc
                                : q + "\n（附图内容：" + desc + "）";
                    }
                    Log.d(TAG, "[" + sessionId + "] 联网+带图：M3 vision 解析并入查询");
                }
                searchText = MiniMaxClient.webSearch(q);
                Log.d(TAG, "[" + sessionId + "] 联网检索完成 " + searchText.length() + "字");
            } catch (Exception e) {
                Log.e(TAG, "[" + sessionId + "] 联网搜索失败，降级为不联网作答", e);
                searchText = null;
            }
        }

        // 装配（含 recall 取证 + 联网搜索结果注入）——当前问题不入图，也不占证据位
        String query = userText.trim().isEmpty() ? memoryText : userText;   // ifBlank
        List<MiniMaxClient.Msg> msgs = assemble(query, history, userText, imageB64,
                webSearch, searchText);

        // 第 2 轮：M3 本地非联网作答（搜索结果+提问+记忆均在 system/窗口中）
        String reply = MiniMaxClient.chat(msgs);
        Log.d(TAG, "[" + sessionId + "] 回复 " + reply.length() + "字: "
                + take(reply.replace('\n', ' '), 100));   // take(100)

        // 整轮写入：提问 → 回答 合成一条记忆事件 + 双侧消息落盘 + 图快照持久化
        String pair = memoryText + "\n→ " + reply;
        int rc = engine.rememberTurn("user", pair, now, "u" + now, sessionId);
        store.appendMessage(sessionId, new Message("user", memoryText, now, imagePath));
        store.appendMessage(sessionId, new Message("assistant", reply, now + 1, null));
        int rcS = engine.save();
        Log.d(TAG, "[" + sessionId + "] rememberTurn(整轮) rc=" + rc
                + " save rc=" + rcS + "，本轮完成");
        return reply;
    }

    // assemble 伪代码：system（人设+档案+recall 证据[+联网搜索结果]）+ 滑窗轮次
    //   （图片轮只带记忆文本）+ 当前用户轮（带图则多模态）；同时估算本轮发送 token
    //   并记录 lastSentTokens。顺带产出 TurnDiag 快照供诊断 tab。
    private List<MiniMaxClient.Msg> assemble(String query, List<Message> history,
                                             String userText, String imageB64,
                                             boolean webSearch, String searchText) {
        // takeLast(windowSize)：取历史末尾至多 windowSize 条
        int from = Math.max(0, history.size() - windowSize);
        List<Message> window = history.subList(from, history.size());
        Set<String> windowTexts = new HashSet<>();
        for (Message m : window) windowTexts.add(m.text);
        PromptResult pr;
        try {
            pr = buildSystemPrompt(query, windowTexts);
        } catch (org.json.JSONException e) {
            // Kotlin 中 JSON 异常非受检、直接外抛；Java 这里包装为 RuntimeException 等价传播
            throw new RuntimeException(e);
        }
        String system = pr.system;
        List<String> evidence = pr.evidence;

        // 联网搜索结果注入 system（仅本轮生效，不落记忆——时效内容入库会污染召回；
        // 截断到 SEARCH_MAX_CHARS，防超长搜索结果撑爆单轮上下文）
        if (searchText != null && !searchText.isEmpty()) {
            if (searchText.length() > SEARCH_MAX_CHARS) {
                Log.d(TAG, "[" + sessionId + "] 联网结果截断 " + searchText.length()
                        + "→" + SEARCH_MAX_CHARS + "字");
                searchText = searchText.substring(0, SEARCH_MAX_CHARS)
                        + "\n……（搜索结果过长已截断）";
            }
            system = system + "\n\n联网搜索结果（实时检索，供本轮作答参考）：\n" + searchText;
        }

        List<MiniMaxClient.Msg> msgs = new ArrayList<>();
        msgs.add(new MiniMaxClient.Msg("system", system));
        for (Message m : window) {
            msgs.add(new MiniMaxClient.Msg(m.role, m.text));  // 证据/窗口均不回传图片
        }
        msgs.add(new MiniMaxClient.Msg(
                "user",
                (imageB64 != null && userText.trim().isEmpty())
                        ? "请描述这张图片" : userText,
                imageB64));

        int tok = 0;
        int withImage = 0;
        for (MiniMaxClient.Msg m : msgs) {
            tok += estimateTokens(m.text);
            if (m.imageB64 != null) withImage++;
        }
        lastSentTokens = tok + withImage * IMAGE_TOKENS;

        Log.d(TAG, "[" + sessionId + "] 装配完成: system=" + system.length() + "字 "
                + "滑窗=" + window.size() + "条 messages=" + msgs.size() + "条 "
                + (searchText != null ? "联网结果=" + searchText.length() + "字 " : "")
                + "~" + lastSentTokens + "tok");
        diagLog.addLast(new TurnDiag(query, system, evidence,
                new ArrayList<>(msgs), webSearch, searchText,
                lastSentTokens, System.currentTimeMillis()));
        while (diagLog.size() > DIAG_MAX) diagLog.removeFirst();
        return msgs;
    }

    // buildSystemPrompt 伪代码：人设（对话/导游）+ 导览方案段（导游模式）+
    //   用户档案槽位 + recall 证据（每条带 [yyyy-MM-dd] 时间戳，供模型做时序推断）；
    //   证据为空时省略对应段。windowTexts=滑窗原文集合——已在滑窗里的证据直接跳过
    //   （否则同一条消息在 system 证据段和滑窗 messages 里出现两次，浪费 token）；
    //   证据内部去重由引擎 rerank 在截断前完成，这里不重复做。
    //   返回 (全文, 实际注入的证据列表)。
    private PromptResult buildSystemPrompt(String query, Set<String> windowTexts)
            throws org.json.JSONException {
        StringBuilder sb = new StringBuilder(
                guideText != null
                        ? "你是一位文旅导游助手，正依据《" + guideTitle + "》为游客提供导览服务。"
                        + "优先依据下面的导览方案回答游客的问题（含拍照提问）；方案没有覆盖的，"
                        + "可补充通用知识并明确说明。回答口语化、条理清晰，每次不超过 150 字。"
                        : "你是一个带长期记忆的对话助手。");

        if (guideText != null && !guideText.trim().isEmpty()) {   // isNotBlank
            sb.append("\n\n导览方案《").append(guideTitle).append("》：");
            if (guideText.length() <= PROMPT_CHARS) {
                sb.append("\n").append(guideText);
            } else {
                sb.append("\n").append(guideText, 0, PROMPT_CHARS);   // take(PROMPT_CHARS)
                sb.append("\n……（方案较长已截断，完整内容见页内「方案」按钮；")
                        .append("更多细节可从下方相关记忆中取证）");
            }
        }

        JSONObject prof = new JSONObject(engine.getProfile());
        JSONArray slots = prof.optJSONArray("slots");
        if (slots != null && slots.length() > 0) {
            sb.append("\n\n用户档案：");
            for (int i = 0; i < slots.length(); i++) {
                JSONObject s = slots.getJSONObject(i);
                sb.append("\n- ").append(s.getString("slot")).append(": ")
                        .append(s.getString("value"));
            }
        }

        List<String> evidence = new ArrayList<>();
        int skipped = 0;
        // 证据内部去重已在引擎 rerank 完成（截断前贪心，重复文本不占名额）；
        // 这里只过滤"滑窗已包含"的证据——滑窗是 App 层概念，引擎不可见。
        String recallRaw = engine.recall(query, recallLimit);
        JSONArray evs = new JSONArray(recallRaw);
        Log.d(TAG, "[" + sessionId + "] recall查询='" + query + "' -> "
                + evs.length() + "条去重后证据");
        for (int i = 0; i < evs.length(); i++) {
            JSONObject e = evs.getJSONObject(i);
            String txt = e.getString("text");
            if (windowTexts.contains(txt)) {   // `in` 集合判断
                skipped++;
                Log.d(TAG, "[" + sessionId + "]   证据[" + (i + 1) + "] 跳过（滑窗已含）: "
                        + take(txt, 60));
                continue;
            }
            String d = fmt.format(new Date(e.getLong("ts") * 1000));
            String line = "- [" + d + "] " + txt;
            evidence.add(line);
            Log.d(TAG, "[" + sessionId + "]   证据[" + evidence.size() + "] score="
                    + e.optDouble("score") + " ts=" + d + " " + take(txt, 80));
        }
        if (skipped > 0) Log.d(TAG, "[" + sessionId + "] 滑窗重叠过滤 " + skipped + " 条");
        if (!evidence.isEmpty()) {
            sb.append("\n\n相关记忆（来自历史对话，可能超出当前上下文窗口）：");
            for (String line : evidence) sb.append("\n").append(line);
        } else {
            Log.w(TAG, "[" + sessionId + "] recall 无有效证据（滑窗过滤后为空）——模型本轮收不到记忆");
        }
        return new PromptResult(sb.toString(), evidence);
    }

    /** Kotlin Pair<String, List<String>> 的等价持有类：system 全文 + 证据列表 */
    private static final class PromptResult {
        final String system;
        final List<String> evidence;

        PromptResult(String system, List<String> evidence) {
            this.system = system;
            this.evidence = evidence;
        }
    }

    /** take(n) 等价：超过 n 字符截断，否则原样返回 */
    private static String take(String s, int n) {
        return s.length() > n ? s.substring(0, n) : s;
    }

    // estimateTokens 伪代码：CJK 字符按 1 token/字、ASCII 按 1 token/4 字符
    //   （混合中英文本的工程近似，仅用于状态栏对比展示，不作计费依据）。
    public static int estimateTokens(String text) {
        int cjk = 0;
        int other = 0;
        for (int i = 0; i < text.length(); i++) {
            if (text.charAt(i) > 0x2E7F) cjk++; else other++;
        }
        return cjk + (other + 3) / 4;
    }
}
