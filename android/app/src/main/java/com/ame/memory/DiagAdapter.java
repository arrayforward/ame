/*
 * 所属模块：app——诊断 tab 的适配器（ChatActivity/GuideActivity 共用）。
 * 主体思路：
 *   列表行：一轮提交的摘要（时间、recall 查询词、发送 token、证据/消息条数）；
 *   详情弹窗：分栏目卡片展示该轮提交给 LLM 的完整上下文——
 *     ① 本轮概况（查询词/时间/token/条数）
 *     ② 记忆证据（逐条编号，无证据时橙色警示——诊断召回问题第一眼可见）
 *     ③ System Prompt 全文（等宽字体卡片）
 *     ④ 提交的 Messages（按角色分色卡片：system 黄 / user 蓝 / assistant 灰，带图标记）
 * 依赖关系：MemoryOrchestrator.TurnDiag / MiniMaxClient.Msg。
 */
package com.ame.memory;

import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import androidx.recyclerview.widget.RecyclerView;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;

public class DiagAdapter extends RecyclerView.Adapter<DiagAdapter.VH> {

    private final List<MemoryOrchestrator.TurnDiag> data;
    private final SimpleDateFormat fmt = new SimpleDateFormat("MM-dd HH:mm:ss", Locale.US);

    public DiagAdapter(List<MemoryOrchestrator.TurnDiag> data) {
        this.data = data;
    }

    public static class VH extends RecyclerView.ViewHolder {
        final TextView title;
        final TextView sub;

        public VH(View v) {
            super(v);
            title = v.findViewById(R.id.tvDiagTitle);
            sub = v.findViewById(R.id.tvDiagSub);
        }
    }

    @Override
    public VH onCreateViewHolder(ViewGroup p, int vt) {
        return new VH(LayoutInflater.from(p.getContext())
                .inflate(R.layout.item_diag, p, false));
    }

    @Override
    public int getItemCount() {
        return data.size();
    }

    @Override
    public void onBindViewHolder(VH h, int pos) {
        MemoryOrchestrator.TurnDiag d = data.get(pos);
        // 列表按时间正序（旧→新），行号 = pos+1
        h.title.setText("第 " + (pos + 1) + " 轮 · " + fmt.format(new Date(d.ts))
                + " · 查询：" + take(d.query, 36));
        h.sub.setText("发送 ~" + d.sentTokens + " tok │ 证据 " + d.evidence.size()
                + " 条 │ 消息 " + d.messages.size() + " 条（含 system）"
                + (d.deepRerank ? " │ 重排：开(60→5)" : ""));
        h.itemView.setOnClickListener(v -> showDetail(v.getContext(), d, pos + 1));
    }

    // ───────────────────────── 详情弹窗（分栏目卡片） ─────────────────────────

    private void showDetail(Context ctx, MemoryOrchestrator.TurnDiag d, int round) {
        LinearLayout col = new LinearLayout(ctx);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(ctx, 12);
        col.setPadding(pad, pad, pad, pad);

        // ① 本轮概况
        addSectionTitle(ctx, col, "本轮概况");
        addCard(ctx, col,
                kv("查询词", d.query) + "\n" +
                kv("时间", fmt.format(new Date(d.ts))) + "\n" +
                kv("联网搜索", d.webSearch ? "开（第1轮检索→第2轮 M3 综合作答）" : "关") + "\n" +
                kv("深度重排", d.deepRerank ? "开(60→5)" : "关") + "\n" +
                kv("发送估算", "~" + d.sentTokens + " tok") + "\n" +
                kv("记忆证据", d.evidence.size() + " 条") + "\n" +
                kv("提交消息", d.messages.size() + " 条（含 system）"));

        // ② 记忆证据（召回效果诊断核心：无证据时橙色警示）
        addSectionTitle(ctx, col, "记忆证据（" + d.evidence.size() + " 条）");
        if (d.evidence.isEmpty()) {
            addCard(ctx, col, "本轮无记忆证据——模型完全靠滑窗与自身知识作答，"
                    + "若问题涉及更早的对话，答案大概率「失忆」。", true);
        } else {
            StringBuilder ev = new StringBuilder();
            for (int i = 0; i < d.evidence.size(); i++) {
                if (i > 0) ev.append("\n\n");
                ev.append("证据 ").append(i + 1).append("｜").append(d.evidence.get(i));
            }
            addCard(ctx, col, ev.toString());
        }

        // ② 联网搜索结果（第 1 轮检索，仅本轮注入，不落记忆）
        if (d.searchText != null && !d.searchText.isEmpty()) {
            addSectionTitle(ctx, col, "联网搜索结果（第 1 轮检索，未入记忆）");
            addCard(ctx, col, d.searchText);
        }

        // ③ System Prompt 全文
        addSectionTitle(ctx, col, "System Prompt（人设/方案/档案/证据段全文）");
        addCard(ctx, col, d.system);

        // ④ 提交的 Messages（system 已在③全文展示，这里从第 2 条起即滑窗+当前轮）
        List<MiniMaxClient.Msg> msgs = d.messages;
        int shown = Math.max(0, msgs.size() - 1);  // 除 system 外的条数
        addSectionTitle(ctx, col, "提交的 Messages（滑窗 " + shown + " 条）");
        for (int i = 1; i < msgs.size(); i++) {
            addMsgCard(ctx, col, msgs.get(i), i);
        }

        ScrollView scroll = new ScrollView(ctx);
        scroll.addView(col);
        new AlertDialog.Builder(ctx)
                .setTitle("第 " + round + " 轮 · 提交上下文")
                .setView(scroll)
                .setPositiveButton("关闭", null)
                .show();
    }

    // addSectionTitle：栏目标题条（浅蓝底、深色粗体字，上下留白）
    private void addSectionTitle(Context ctx, LinearLayout parent, String title) {
        TextView tv = new TextView(ctx);
        tv.setText(title);
        tv.setTextSize(14f);
        tv.setTypeface(null, Typeface.BOLD);
        tv.setTextColor(Color.parseColor("#1A237E"));
        tv.setBackgroundColor(Color.parseColor("#E8EAF6"));
        int h = dp(ctx, 8);
        tv.setPadding(h, h, h, h);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dp(ctx, 10), 0, dp(ctx, 4));
        parent.addView(tv, lp);
    }

    // addCard：灰底圆角内容卡片（等宽小字，长文本自动换行）
    private void addCard(Context ctx, LinearLayout parent, String content) {
        addCard(ctx, parent, content, false);
    }

    private void addCard(Context ctx, LinearLayout parent, String content, boolean warn) {
        TextView tv = new TextView(ctx);
        tv.setText(content);
        tv.setTextSize(12f);
        tv.setTypeface(Typeface.MONOSPACE);
        tv.setTextColor(warn ? Color.parseColor("#B26A00")
                             : Color.parseColor("#333333"));
        tv.setBackgroundColor(warn ? Color.parseColor("#FFF8E1")
                                   : Color.parseColor("#FAFAFA"));
        int p = dp(ctx, 8);
        tv.setPadding(p, p, p, p);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        parent.addView(tv, lp);
    }

    // addMsgCard：单条消息卡片——顶部角色徽章（分色）+ 正文
    private void addMsgCard(Context ctx, LinearLayout parent, MiniMaxClient.Msg m, int idx) {
        LinearLayout card = new LinearLayout(ctx);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundColor(Color.parseColor("#FAFAFA"));
        int p = dp(ctx, 8);
        card.setPadding(p, p, p, p);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, 0, 0, dp(ctx, 6));
        parent.addView(card, lp);

        TextView badge = new TextView(ctx);
        String label = "#" + idx + "  " + m.role + (m.imageB64 != null ? " · 带图" : "");
        badge.setText(label);
        badge.setTextSize(11f);
        badge.setTypeface(null, Typeface.BOLD);
        badge.setTextColor(Color.parseColor("#222222"));
        badge.setBackgroundColor(roleColor(m.role));
        badge.setPadding(p, dp(ctx, 2), p, dp(ctx, 2));
        card.addView(badge);

        TextView body = new TextView(ctx);
        body.setText(take(m.text, 4000));
        body.setTextSize(12f);
        body.setTextColor(Color.parseColor("#333333"));
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        blp.setMargins(0, dp(ctx, 4), 0, 0);
        card.addView(body, blp);
    }

    private int roleColor(String role) {
        switch (role) {
            case "system":    return Color.parseColor("#FFF3CD");  // 黄
            case "user":      return Color.parseColor("#DCE3FF");  // 蓝
            default:          return Color.parseColor("#F2F2F2");  // 灰（assistant 等）
        }
    }

    private String kv(String k, String v) {
        return k + "：" + v;
    }

    private static int dp(Context ctx, int v) {
        return (int) (v * ctx.getResources().getDisplayMetrics().density + 0.5f);
    }

    /** take(n) 等价：超过 n 字符截断，否则原样返回 */
    private static String take(String s, int n) {
        return s.length() > n ? s.substring(0, n) : s;
    }
}
