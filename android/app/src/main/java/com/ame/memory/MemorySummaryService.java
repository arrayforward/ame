/*
 * 所属模块：app——记忆摘要服务（LLM 记忆整理，三级 JSON + 联想词）。
 * 主体思路：
 *   注入知识/检索知识/用户上下文对话先经 LLM 整理成结构化摘要，避免一次性把
 *   复杂原始数据塞给模型；数据分三层：
 *     L1 基础摘要（几十字，言简意赅，直接服务当前问答）；
 *     L2 详细整理（难题参考要点）+ 联想词（Java 侧可做联想检索）；
 *     L3 原始知识文本。
 *   完整 JSON 以 "MEMSUM:" 前缀作为事件文本写入 AME（speaker=summary），
 *   recall 后过滤前缀即还原完整三级 JSON；注入上下文时调用方自选层级（1/2/3）。
 * 接口（对应需求四个封装点 + 联想检索 + 分层装配）：
 *   summarize()        LLM 生成摘要（JSON 三级+联想词；失败降级为截断摘要并记日志）
 *   storeToAme()       完整 JSON 写入 AME
 *   fetchFromAme()     从 AME 取回完整 JSON（三级+联想词）
 *   fetchByKeywords()  用 L2 联想词在 Java 侧逐个 recall（联想检索）
 *   buildContext()     按用户选择层级（1=L1 / 2=L1+L2 / 3=全量）装配注入文本
 * 依赖关系：MiniMaxClient（LLM）、AmeEngine（recall/rememberTurn）、org.json。
 *   全部方法同步阻塞，调用方负责切 io 线程（与 MemoryOrchestrator 同约定）。
 */
package com.ame.memory;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;

public final class MemorySummaryService {

    private static final String TAG = "AmeMem";
    /** AME 事件文本前缀：标识该事件为三级摘要 JSON（检索时据此过滤还原） */
    public static final String MARK = "MEMSUM:";

    /** 三级摘要 + 联想词的结构化载体 */
    public static final class Summary {
        public String l1 = "";                  // 基础摘要（几十字）
        public String l2 = "";                  // 详细整理（难题参考）
        public List<String> keywords = new ArrayList<>();  // 联想词（Java 联想检索用）
        public String l3 = "";                  // 原始知识文本

        /** 完整 JSON（三级+联想词），存取 AME 的统一格式 */
        public String toJson() {
            try {
                return new JSONObject()
                        .put("l1", l1)
                        .put("l2", l2)
                        .put("keywords", new JSONArray(keywords))
                        .put("l3", l3)
                        .toString();
            } catch (JSONException e) {
                throw new RuntimeException(e);
            }
        }

        /** 从完整 JSON 还原；字段缺失按空串/空表容错 */
        public static Summary fromJson(String json) {
            Summary s = new Summary();
            try {
                JSONObject jo = new JSONObject(json);
                s.l1 = jo.optString("l1");
                s.l2 = jo.optString("l2");
                s.l3 = jo.optString("l3");
                JSONArray ka = jo.optJSONArray("keywords");
                if (ka != null) {
                    for (int i = 0; i < ka.length(); i++) s.keywords.add(ka.optString(i));
                }
            } catch (JSONException e) {
                throw new RuntimeException(e);
            }
            return s;
        }
    }

    private final AmeEngine engine;
    private final String sessionId;

    public MemorySummaryService(AmeEngine engine, String sessionId) {
        this.engine = engine;
        this.sessionId = sessionId;
    }

    /** ① LLM 记忆整理：source=注入知识/检索知识/对话上下文；question=当前问题（可空）。
     *  输出严格 JSON 三级+联想词；LLM/解析失败降级为截断摘要（不阻断主流程）。 */
    public Summary summarize(String source, String question) {
        String system = "你是记忆整理器。把给定材料整理成严格 JSON（不要输出任何其他文字）："
                + "{\"l1\":\"不超过60字的基础摘要，言简意赅\","
                + "\"l2\":\"不超过300字的详细整理，面向难以理解的问题给出要点、因果和时序\","
                + "\"keywords\":[\"3-6个联想词，用于扩展检索相关知识，名词优先\"]}。"
                + "要求：l1 必须提炼关键事实（人物/时间/地点/数字/结论），不得只复述首句；"
                + "keywords 必填且不得为空数组。";
        String user = (question == null || question.isEmpty() ? "" : "当前问题：" + question + "\n")
                + "材料：\n" + source;
        Summary s = new Summary();
        s.l3 = source;
        try {
            List<MiniMaxClient.Msg> msgs = new ArrayList<>();
            msgs.add(new MiniMaxClient.Msg("system", system));
            msgs.add(new MiniMaxClient.Msg("user", user));
            String resp = MiniMaxClient.chat(msgs, 4096);
            // 容错剥离：去代码围栏，取首个 { 到末个 } 之间的 JSON
            String text = resp.trim();
            if (text.startsWith("```")) {
                int nl = text.indexOf('\n');
                if (nl >= 0) text = text.substring(nl + 1);
            }
            int a = text.indexOf('{'), b = text.lastIndexOf('}');
            if (a < 0 || b <= a) throw new RuntimeException("摘要响应无 JSON 对象");
            Summary parsed = Summary.fromJson(text.substring(a, b + 1));
            parsed.l3 = source;  // L3 永远保留调用方原文，不依赖模型复述
            return parsed;
        } catch (RuntimeException e) {
            Log.w(TAG, "[" + sessionId + "] 摘要生成失败，降级截断摘要: " + e);
            s.l1 = source.length() > 60 ? source.substring(0, 60) : source;
            s.l2 = source.length() > 300 ? source.substring(0, 300) : source;
            return s;
        }
    }

    /** ② 存储到 AME：完整 JSON（三级+联想词）加 MEMSUM 前缀作为单事件写入 */
    public boolean storeToAme(Summary s, long ts) {
        int rc = engine.rememberTurn("summary", MARK + s.toJson(), ts, "sum" + ts, sessionId);
        Log.d(TAG, "[" + sessionId + "] 摘要入图 rc=" + rc
                + "（L1=" + s.l1.length() + "字 L2=" + s.l2.length() + "字 联想词=" + s.keywords.size() + "）");
        return rc == 0;
    }

    /** ③ 从 AME 取回完整 JSON：recall 后过滤 MEMSUM 事件并解析（非摘要事件忽略） */
    public List<Summary> fetchFromAme(String query, int limit) {
        List<Summary> out = new ArrayList<>();
        try {
            JSONArray evs = new JSONArray(engine.recall(query, limit));
            for (int i = 0; i < evs.length(); i++) {
                String text = evs.getJSONObject(i).optString("text");
                if (!text.startsWith(MARK)) continue;
                try {
                    out.add(Summary.fromJson(text.substring(MARK.length())));
                } catch (RuntimeException e) {
                    Log.w(TAG, "[" + sessionId + "] 摘要 JSON 解析失败，跳过: " + e);
                }
            }
        } catch (JSONException | RuntimeException e) {
            Log.w(TAG, "[" + sessionId + "] 摘要检索失败: " + e);
        }
        Log.d(TAG, "[" + sessionId + "] 摘要检索 '" + query + "' -> " + out.size() + " 条");
        return out;
    }

    /** ④ 联想检索：用 L2 联想词逐个 recall，按完整 JSON 去重（保持首次命中顺序） */
    public List<Summary> fetchByKeywords(List<String> keywords, int limitPerKeyword) {
        LinkedHashSet<String> seen = new LinkedHashSet<>();
        List<Summary> out = new ArrayList<>();
        for (String kw : keywords) {
            if (kw == null || kw.trim().isEmpty()) continue;
            for (Summary s : fetchFromAme(kw.trim(), limitPerKeyword)) {
                if (seen.add(s.toJson())) out.add(s);
            }
        }
        return out;
    }

    /** ⑤ 分层装配：level 1=仅 L1；2=L1+L2（含联想词）；3=全量（含 L3 原文）。
     *  用户可选不提供（level 0 或 items 为空 → 返回空串，不占上下文）。 */
    public String buildContext(List<Summary> items, int level) {
        if (items == null || items.isEmpty() || level <= 0) return "";
        StringBuilder sb = new StringBuilder();
        for (Summary s : items) {
            if (!s.l1.isEmpty()) sb.append("【摘要】").append(s.l1).append('\n');
            if (level >= 2) {
                if (!s.l2.isEmpty()) sb.append("【详述】").append(s.l2).append('\n');
                if (!s.keywords.isEmpty())
                    sb.append("【联想词】").append(String.join("、", s.keywords)).append('\n');
            }
            if (level >= 3 && !s.l3.isEmpty()) sb.append("【原文】").append(s.l3).append('\n');
            sb.append('\n');
        }
        return sb.toString();
    }
}
