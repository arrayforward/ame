/*
 * 所属模块：app——记忆编排器（上下文外置的核心逻辑）。
 * 主体思路（每轮问答流水线）：
 *   步骤1：用户轮写入 AME（remember_turn，user）；图片轮的记忆文本 = "[图片] " +
 *     随图文字，无随图文字时调 M3 vision 生成一句话描述（失败存占位文本）——
 *     让图片语义可被检索（图片本身不进记忆图）；
 *   步骤2：装配上下文 = 用户档案槽位（getProfile）+ AME recall 证据（带时间戳，
 *     滑窗外的历史只从记忆图取证，不做压缩摘要——"外置而非压缩"模式）
 *     + 最近 z 轮原文（滑动窗口，窗口内图片轮只带其记忆文本）；
 *   步骤3：MiniMax 整段作答（本轮带图则走多模态 content 数组）；
 *   步骤4：助手轮写入 AME + 消息落 jsonl + 引擎快照落盘。
 * 导游模式（guideTitle/guideText 非空）：人设切换为导游助手，导览方案文本注入
 *   system prompt（超 PROMPT_CHARS 截断，截断外细节靠导入时灌入记忆图的方案块
 *   经 recall 取证）；记忆机制与普通对话完全一致（每 session 独立、删即清）。
 * 关键数据结构：token 估算口径——CJK 字符 1 字≈1 token、ASCII 4 字符≈1 token、
 *   图片按定值 1500 token/张（最长边≤1024 JPEG 的经验口径，注释于估算函数）。
 * 依赖关系：AmeEngine（JNI）、MiniMaxClient、SessionStore、BuildConfig（可调上限）。
 */
package com.ame.memory

import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MemoryOrchestrator(
    private val engine: AmeEngine,
    private val store: SessionStore,
    private val sessionId: String,
    private val windowSize: Int = BuildConfig.CONTEXT_WINDOW_TURNS,  // 滑窗轮数（可调）
    private val recallLimit: Int = BuildConfig.RECALL_TOP_K,         // 证据条数（可调）
    // 导游模式：非空时切换导游人设，并把导览方案文本注入 system prompt
    // （超出 PROMPT_CHARS 截断；截断外的细节靠导入时灌入记忆图的方案块 recall 取证）
    private val guideTitle: String? = null,
    private val guideText: String? = null
) {
    private val fmt = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US)

    /** 上一轮实际发送的上下文 token 估算（状态栏显示用） */
    var lastSentTokens: Int = 0
        private set

    /** 引擎统计（JNI 透传）：{turns,total_chars,events,nodes} */
    fun statsJson(): String = engine.stats()

    /** 一轮问答：返回助手回复文本；全程同步，调用方负责切线程 */
    fun chat(userText: String, imageB64: String? = null,
             imagePath: String? = null): String {
        val now = System.currentTimeMillis() / 1000
        val history = store.loadMessages(sessionId)

        // 步骤1：用户轮写入记忆（图片轮存记忆文本而非图片本体）
        val memoryText = when {
            imageB64 == null -> userText
            userText.isNotBlank() -> "[图片] $userText"
            else -> "[图片] " + MiniMaxClient.visionCaption(imageB64)
                .ifBlank { "用户分享了一张图片" }
        }
        engine.rememberTurn("user", memoryText, now, "u$now", sessionId)

        // 步骤2+3：装配并作答（recall 查询用随图文字；纯图轮用记忆文本查）
        val query = userText.ifBlank { memoryText }
        val msgs = assemble(query, history, userText, imageB64)
        val reply = MiniMaxClient.chat(msgs)

        // 步骤4：助手轮写入记忆 + 双侧消息落盘 + 图快照持久化
        engine.rememberTurn("assistant", reply, now + 1, "a$now", sessionId)
        store.appendMessage(sessionId, Message("user", memoryText, now, imagePath))
        store.appendMessage(sessionId, Message("assistant", reply, now + 1))
        engine.save()
        return reply
    }

    // assemble 伪代码：system（人设+档案+recall 证据）+ 滑窗轮次（图片轮只带记忆文本）
    //   + 当前用户轮（带图则多模态）；同时估算本轮发送 token 并记录 lastSentTokens。
    private fun assemble(query: String, history: List<Message>, userText: String,
                         imageB64: String?): List<MiniMaxClient.Msg> {
        val msgs = mutableListOf<MiniMaxClient.Msg>()
        msgs.add(MiniMaxClient.Msg("system", buildSystemPrompt(query)))
        for (m in history.takeLast(windowSize))
            msgs.add(MiniMaxClient.Msg(m.role, m.text))  // 证据/窗口均不回传图片
        msgs.add(MiniMaxClient.Msg(
            "user",
            if (imageB64 != null && userText.isBlank()) "请描述这张图片" else userText,
            imageB64))
        lastSentTokens = msgs.sumOf { estimateTokens(it.text) } +
                msgs.count { it.imageB64 != null } * IMAGE_TOKENS
        return msgs
    }

    // buildSystemPrompt 伪代码：人设（对话/导游）+ 导览方案段（导游模式）+
    //   用户档案槽位 + recall 证据（每条带 [yyyy-MM-dd] 时间戳，供模型做时序推断）；
    //   证据为空时省略对应段。
    private fun buildSystemPrompt(query: String): String {
        val sb = StringBuilder(
            if (guideText != null)
                "你是一位文旅导游助手，正依据《$guideTitle》为游客提供导览服务。" +
                "优先依据下面的导览方案回答游客的问题（含拍照提问）；方案没有覆盖的，" +
                "可补充通用知识并明确说明。回答口语化、条理清晰，每次不超过 150 字。"
            else
                "你是一个带长期记忆的对话助手。")

        if (guideText != null && guideText.isNotBlank()) {
            sb.append("\n\n导览方案《").append(guideTitle).append("》：")
            if (guideText.length <= PROMPT_CHARS) {
                sb.append("\n").append(guideText)
            } else {
                sb.append("\n").append(guideText.take(PROMPT_CHARS))
                sb.append("\n……（方案较长已截断，完整内容见页内「方案」按钮；")
                    .append("更多细节可从下方相关记忆中取证）")
            }
        }

        val prof = JSONObject(engine.getProfile())
        val slots = prof.optJSONArray("slots")
        if (slots != null && slots.length() > 0) {
            sb.append("\n\n用户档案：")
            for (i in 0 until slots.length()) {
                val s = slots.getJSONObject(i)
                sb.append("\n- ").append(s.getString("slot")).append(": ")
                    .append(s.getString("value"))
            }
        }

        val evs = JSONArray(engine.recall(query, recallLimit))
        if (evs.length() > 0) {
            sb.append("\n\n相关记忆（来自历史对话，可能超出当前上下文窗口）：")
            for (i in 0 until evs.length()) {
                val e = evs.getJSONObject(i)
                val d = fmt.format(Date(e.getLong("ts") * 1000))
                sb.append("\n- [").append(d).append("] ").append(e.getString("text"))
            }
        }
        return sb.toString()
    }

    companion object {
        /** 图片 token 估算定值（最长边≤1024 的 JPEG，经验口径 ~1500 token/张） */
        const val IMAGE_TOKENS = 1500

        /** 导览方案注入 system prompt 的字符上限（M3 256k 上下文下的保守口径） */
        const val PROMPT_CHARS = 8000

        // estimateTokens 伪代码：CJK 字符按 1 token/字、ASCII 按 1 token/4 字符
        //   （混合中英文本的工程近似，仅用于状态栏对比展示，不作计费依据）。
        fun estimateTokens(text: String): Int {
            var cjk = 0
            var other = 0
            for (c in text) if (c.code > 0x2E7F) cjk++ else other++
            return cjk + (other + 3) / 4
        }
    }
}
