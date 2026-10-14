/*
 * 所属模块：app——MiniMax API 客户端（OpenAI 兼容 /v1/chat/completions）。
 * 主体思路：OkHttp 同步 POST（整段回复，简单可靠优先），调用方负责切后台线程；
 *   支持多模态消息（content 数组：text + image_url data URI，M3 vision 实测可用）；
 *   M3 是推理模型，content 内联 <think>...</think>——stripThink 剥离后再展示/入库；
 *   max_tokens 口径：含思考预算（usage 有 reasoning_tokens 字段），默认给 2048 防截断。
 *   key 从 BuildConfig（local.properties 注入）读，不落日志。
 * 依赖关系：okhttp3 / org.json；BuildConfig 由 app/build.gradle 生成。
 */
package com.ame.memory;

import android.util.Log;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.regex.Pattern;

import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;
import org.json.JSONArray;
import org.json.JSONObject;

public final class MiniMaxClient {

    private static final String TAG = "AmeMem";

    private static final OkHttpClient client = new OkHttpClient.Builder()
            .connectTimeout(30, TimeUnit.SECONDS)
            .readTimeout(180, TimeUnit.SECONDS)
            .build();

    /** 思考段正则（DOTALL 等价：跨行匹配），用于 stripThink 剥离 */
    private static final Pattern THINK_PATTERN =
            Pattern.compile("<think>.*?</think>\\s*", Pattern.DOTALL);

    private MiniMaxClient() {
    }

    /** 一条消息：text 必有；imageB64 非空时走多模态 content 数组 */
    public static final class Msg {
        public final String role;
        public final String text;
        public final String imageB64;   // null = 纯文本轮

        public Msg(String role, String text) {
            this(role, text, null);
        }

        public Msg(String role, String text, String imageB64) {
            this.role = role;
            this.text = text;
            this.imageB64 = imageB64;
        }
    }

    /** 剥离 M3 内联思考段（<think>...</think>），返回展示/入库用的干净文本 */
    public static String stripThink(String s) {
        return THINK_PATTERN.matcher(s).replaceAll("").trim();
    }

    /** 整段对话补全；失败抛异常（含 HTTP 码），由调用方展示 */
    public static String chat(List<Msg> messages) {
        return chat(messages, 2048);
    }

    /** 整段对话补全（可指定 maxTokens）；失败抛异常（含 HTTP 码），由调用方展示 */
    public static String chat(List<Msg> messages, int maxTokens) {
        String body;
        try {
            JSONArray arr = new JSONArray();
            for (Msg m : messages) {
                Object content;
                if (m.imageB64 != null) {
                    // OpenAI 兼容多模态格式（MiniMax-M3 实测 HTTP 200 可读图）
                    content = new JSONArray()
                            .put(new JSONObject().put("type", "text").put("text", m.text))
                            .put(new JSONObject().put("type", "image_url").put(
                                    "image_url",
                                    new JSONObject().put("url", "data:image/jpeg;base64," + m.imageB64)));
                } else {
                    content = m.text;
                }
                arr.put(new JSONObject().put("role", m.role).put("content", content));
            }
            body = new JSONObject()
                    .put("model", BuildConfig.MINIMAX_MODEL)
                    .put("max_tokens", maxTokens)  // M3：max_tokens 含思考预算，给足防截断
                    .put("messages", arr)
                    .toString();
        } catch (org.json.JSONException e) {
            // Kotlin 中 JSON 异常非受检；Java 这里包装为 RuntimeException 等价传播
            throw new RuntimeException(e);
        }
        // trimEnd('/') 等价：去掉末尾的 '/'
        String base = BuildConfig.MINIMAX_BASE_URL;
        while (base.endsWith("/")) base = base.substring(0, base.length() - 1);
        Request req = new Request.Builder()
                .url(base + "/chat/completions")
                .header("Authorization", "Bearer " + BuildConfig.MINIMAX_API_KEY)
                .header("Content-Type", "application/json")
                .post(RequestBody.create(
                        body, MediaType.parse("application/json; charset=utf-8")))
                .build();
        try (Response resp = client.newCall(req).execute()) {
            String text = resp.body() != null ? resp.body().string() : "";
            if (!resp.isSuccessful()) {
                // take(300)：最多取前 300 字符
                String snippet = text.length() > 300 ? text.substring(0, 300) : text;
                Log.e(TAG, "MiniMax HTTP " + resp.code() + ": " + snippet);
                throw new RuntimeException("MiniMax HTTP " + resp.code());
            }
            JSONObject jo;
            try {
                jo = new JSONObject(text);
            } catch (org.json.JSONException e) {
                throw new RuntimeException(e);
            }
            JSONObject usage = jo.optJSONObject("usage");
            Log.d(TAG, "MiniMax 200: model=" + BuildConfig.MINIMAX_MODEL
                    + " maxTokens=" + maxTokens + " msgs=" + messages.size()
                    + " usage=" + (usage != null ? usage.toString() : "无"));
            String content;
            try {
                content = jo.getJSONArray("choices").getJSONObject(0)
                        .getJSONObject("message").getString("content");
            } catch (org.json.JSONException e) {
                throw new RuntimeException(e);
            }
            return stripThink(content);
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
    }

    /** 供联网搜索用的图片解析提示词：比记忆入库的一句话描述更详细，便于搜索命中 */
    private static final String VISION_SEARCH_PROMPT =
            "详细描述这张图片的关键信息（主体、文字、场景、品牌等），不超过 80 字，供联网搜索使用。";

    /** 图片一句话描述（记忆入库用文本，让图片语义可检索）；失败返回空串由调用方兜底 */
    public static String visionCaption(String imageB64) {
        return visionCaption(imageB64, "用一句话描述这张图片的内容，不超过 40 字。");
    }

    /** 图片解析（自定义提示词版）；失败返回空串由调用方兜底 */
    private static String visionCaption(String imageB64, String prompt) {
        try {
            List<Msg> msgs = java.util.Collections.singletonList(
                    new Msg("user", prompt, imageB64));
            return chat(msgs, 512);
        } catch (Exception e) {
            return "";
        }
    }

    /** 供联网搜索用的图片解析（详细描述关键信息，便于搜索命中）；失败返回空串 */
    public static String visionForSearch(String imageB64) {
        return visionCaption(imageB64, VISION_SEARCH_PROMPT);
    }

    /** 联网检索查询内容（MiniMax Server Tool web_search）：走 OpenAI Responses API
     *  （{BASE_URL}/responses），声明 tools=[{"type":"web_search"}] 后服务端单次请求内
     *  完成"搜索+基于结果作答"，读顶层 output_text 作为查询内容（含引用来源）。
     *  注意：只发"搜索 system + 查询"——带对话历史时，历史里助手曾说过的陈旧结论
     *  会带偏模型使其放弃调用搜索工具（实测复现）。本方法只负责"查内容"，综合
     *  提问与记忆作答是调用方的第二次 M3 调用。 */
    public static String webSearch(String query) {
        String body;
        try {
            JSONArray arr = new JSONArray()
                    .put(new JSONObject().put("role", "system").put("content",
                            "你是联网搜索助手：必须调用 web_search 工具检索最新信息，"
                                    + "再基于搜索结果简洁作答，回答中注明信息来源，"
                                    + "严格控制在 100 字以内。"))
                    .put(new JSONObject().put("role", "user").put("content", query));
            body = new JSONObject()
                    .put("model", BuildConfig.MINIMAX_MODEL)
                    .put("input", arr)
                    .put("tools", new JSONArray()
                            .put(new JSONObject().put("type", "web_search")))
                    .toString();
        } catch (org.json.JSONException e) {
            throw new RuntimeException(e);
        }
        String base = BuildConfig.MINIMAX_BASE_URL;
        while (base.endsWith("/")) base = base.substring(0, base.length() - 1);
        Request req = new Request.Builder()
                .url(base + "/responses")
                .header("Authorization", "Bearer " + BuildConfig.MINIMAX_API_KEY)
                .header("Content-Type", "application/json")
                .post(RequestBody.create(
                        body, MediaType.parse("application/json; charset=utf-8")))
                .build();
        try (Response resp = client.newCall(req).execute()) {
            String text = resp.body() != null ? resp.body().string() : "";
            if (!resp.isSuccessful()) {
                String snippet = text.length() > 300 ? text.substring(0, 300) : text;
                Log.e(TAG, "webSearch HTTP " + resp.code() + ": " + snippet);
                throw new RuntimeException("webSearch HTTP " + resp.code());
            }
            JSONObject jo;
            try {
                jo = new JSONObject(text);
            } catch (org.json.JSONException e) {
                throw new RuntimeException(e);
            }
            // 首选顶层 output_text；缺失时从 output 数组拼 message 文本兜底
            String out = jo.optString("output_text", "");
            if (out.isEmpty()) {
                StringBuilder sb = new StringBuilder();
                try {
                    JSONArray outputs = jo.optJSONArray("output");
                    if (outputs != null) {
                        for (int i = 0; i < outputs.length(); i++) {
                            JSONObject item = outputs.getJSONObject(i);
                            if (!"message".equals(item.optString("type"))) continue;
                            JSONArray contents = item.optJSONArray("content");
                            if (contents == null) continue;
                            for (int j = 0; j < contents.length(); j++) {
                                String t = contents.getJSONObject(j).optString("text", "");
                                if (!t.isEmpty()) {
                                    if (sb.length() > 0) sb.append('\n');
                                    sb.append(t);
                                }
                            }
                        }
                    }
                } catch (org.json.JSONException ignored) {
                    // 兜底解析失败走下方统一报错
                }
                out = sb.toString();
            }
            if (out.isEmpty()) throw new RuntimeException("webSearch 响应无 output_text");
            Log.d(TAG, "webSearch 200: model=" + BuildConfig.MINIMAX_MODEL
                    + " 结果" + out.length() + "字");
            return stripThink(out);
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
    }
}
