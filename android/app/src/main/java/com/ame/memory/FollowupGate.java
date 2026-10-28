/*
 * 所属模块：app——追问判定器 + 会话阶段状态机（渐进式上下文注入）。
 * 主体思路：AME 引擎只负责检索，"用户是否在追问"由编排层判定：
 *   SIMPLE（首问/新话题）→ 只注入 L1 摘要（省 token，联想词后台预取）；
 *   DEEP（追问）→ 注入联想词检索结果 + L2 详述（不给原文，控制成本）。
 * 判定规则（零 LLM）：有前轮 + 短句追问标记（那/还有/详细/为什么/呢/吗等），
 *   或与上轮共享内容词（CJK 二元组 + ASCII 词重合）；
 *   显式"换个话题/新话题"或长句零重合 → 判定换题回 SIMPLE。
 * 依赖关系：仅 java.util；MemoryOrchestrator.assemble 每轮调用 onTurn(query)。
 */
package com.ame.memory;

import java.util.HashSet;
import java.util.Set;

public final class FollowupGate {

    public enum Stage { SIMPLE, DEEP }

    private Stage stage = Stage.SIMPLE;
    private String lastQuery = "";

    public Stage stage() {
        return stage;
    }

    /** 每轮调用：返回本轮应处阶段，并推进会话状态 */
    public Stage onTurn(String query) {
        String q = query == null ? "" : query.trim();
        if (isFollowup(q, lastQuery)) {
            stage = Stage.DEEP;
        } else if (isNewTopic(q, lastQuery)) {
            stage = Stage.SIMPLE;
        }
        lastQuery = q;
        return stage;
    }

    /** 追问判定：有前轮 +（短句追问标记 或 同主题内容重合） */
    public static boolean isFollowup(String q, String prev) {
        if (prev == null || prev.isEmpty() || q.isEmpty()) return false;
        if (q.contains("换个话题") || q.contains("新话题")) return false;
        if (q.length() <= 30) {
            String[] marks = {"那", "还有", "继续", "详细", "具体", "为什么", "怎么办", "呢", "吗", "再"};
            for (String m : marks)
                if (q.contains(m)) return true;
        }
        return q.length() <= 40 && overlap(q, prev) >= 2;
    }

    /** 换题判定：显式标记，或长句与上轮零重合且无疑问/追问语气 */
    private static boolean isNewTopic(String q, String prev) {
        if (prev == null || prev.isEmpty() || q.isEmpty()) return false;
        if (q.contains("换个话题") || q.contains("新话题")) return true;
        return q.length() > 12 && overlap(q, prev) == 0 && !q.contains("呢") && !q.contains("吗");
    }

    /** 内容重合度：CJK 二元组 + ASCII 词（len≥3）共享数 */
    static int overlap(String a, String b) {
        Set<String> ta = tokens(a), tb = tokens(b);
        int n = 0;
        for (String t : ta)
            if (tb.contains(t)) n++;
        return n;
    }

    private static Set<String> tokens(String s) {
        Set<String> out = new HashSet<>();
        StringBuilder ascii = new StringBuilder();
        StringBuilder cjk = new StringBuilder();
        Runnable flushAscii = () -> {
            if (ascii.length() >= 3) out.add(ascii.toString().toLowerCase());
            ascii.setLength(0);
        };
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c < 128 && Character.isLetterOrDigit(c)) {
                ascii.append(c);
            } else {
                flushAscii.run();
                if (c >= 0x4E00 && c <= 0x9FFF) {
                    cjk.append(c);
                    if (cjk.length() >= 2) {
                        out.add(cjk.substring(cjk.length() - 2));
                        cjk.deleteCharAt(0);
                    }
                } else {
                    cjk.setLength(0);
                }
            }
        }
        flushAscii.run();
        return out;
    }
}
