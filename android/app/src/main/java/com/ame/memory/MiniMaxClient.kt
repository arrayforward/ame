/*
 * 所属模块：app——MiniMax API 客户端（OpenAI 兼容 /v1/chat/completions）。
 * 主体思路：OkHttp 同步 POST（整段回复，简单可靠优先），调用方负责切后台线程；
 *   支持多模态消息（content 数组：text + image_url data URI，M3 vision 实测可用）；
 *   M3 是推理模型，content 内联 <think>...</think>——stripThink 剥离后再展示/入库；
 *   max_tokens 口径：含思考预算（usage 有 reasoning_tokens 字段），默认给 2048 防截断。
 *   key 从 BuildConfig（local.properties 注入）读，不落日志。
 * 依赖关系：okhttp3 / org.json；BuildConfig 由 app/build.gradle 生成。
 */
package com.ame.memory

import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.TimeUnit

object MiniMaxClient {
    private val client = OkHttpClient.Builder()
        .connectTimeout(30, TimeUnit.SECONDS)
        .readTimeout(180, TimeUnit.SECONDS)
        .build()

    /** 一条消息：text 必有；imageB64 非空时走多模态 content 数组 */
    data class Msg(val role: String, val text: String, val imageB64: String? = null)

    /** 剥离 M3 内联思考段（<think>...</think>），返回展示/入库用的干净文本 */
    fun stripThink(s: String): String =
        s.replace(Regex("<think>.*?</think>\\s*", setOf(RegexOption.DOT_MATCHES_ALL)), "").trim()

    /** 整段对话补全；失败抛异常（含 HTTP 码），由调用方展示 */
    fun chat(messages: List<Msg>, maxTokens: Int = 2048): String {
        val arr = JSONArray()
        for (m in messages) {
            val content: Any = if (m.imageB64 != null) {
                // OpenAI 兼容多模态格式（MiniMax-M3 实测 HTTP 200 可读图）
                JSONArray()
                    .put(JSONObject().put("type", "text").put("text", m.text))
                    .put(JSONObject().put("type", "image_url").put(
                        "image_url",
                        JSONObject().put("url", "data:image/jpeg;base64," + m.imageB64)))
            } else {
                m.text
            }
            arr.put(JSONObject().put("role", m.role).put("content", content))
        }
        val body = JSONObject()
            .put("model", BuildConfig.MINIMAX_MODEL)
            .put("max_tokens", maxTokens)  // M3：max_tokens 含思考预算，给足防截断
            .put("messages", arr)
            .toString()
        val req = Request.Builder()
            .url(BuildConfig.MINIMAX_BASE_URL.trimEnd('/') + "/chat/completions")
            .header("Authorization", "Bearer " + BuildConfig.MINIMAX_API_KEY)
            .header("Content-Type", "application/json")
            .post(body.toRequestBody("application/json; charset=utf-8".toMediaType()))
            .build()
        client.newCall(req).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) throw RuntimeException("MiniMax HTTP ${resp.code}")
            return stripThink(
                JSONObject(text).getJSONArray("choices").getJSONObject(0)
                    .getJSONObject("message").getString("content"))
        }
    }

    /** 图片一句话描述（记忆入库用文本，让图片语义可检索）；失败返回空串由调用方兜底 */
    fun visionCaption(imageB64: String): String = try {
        chat(listOf(Msg("user", "用一句话描述这张图片的内容，不超过 40 字。", imageB64)),
             maxTokens = 512)
    } catch (e: Exception) {
        ""
    }
}
