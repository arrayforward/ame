/*
 * 所属模块：app——session 列表页（入口 Activity），底部双 tab：对话 / 导游。
 * 主体思路：
 *   - 对话 tab：原有普通对话 session 列表，新建=直接建 session；
 *   - 导游 tab：每个 session 绑定一份导览方案 PDF（选 PDF → 建 session → 解析
 *     文本/图片 → 切块灌入 AME 记忆图）；点击进入 GuideActivity；
 *   - 两个 tab 的 session 按 type 隔离，各自独立记忆；删除 session 目录递归清除
 *     （消息/快照/导览方案），即"换游客开新 session、删 session 清记忆"。
 * 依赖关系：SessionStore / ChatActivity / GuideActivity / GuidePdf / AmeEngine。
 */
package com.ame.memory;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.Executors;
import java.util.concurrent.ExecutorService;

public class MainActivity extends AppCompatActivity {

    /** 方案切块大小（字符）：够 recall 命中片段即可，块越小图越细但事件越多 */
    private static final int GUIDE_CHUNK_CHARS = 200;
    /** 方案最多灌入记忆图的块数（超大 PDF 的护栏） */
    private static final int GUIDE_MAX_CHUNKS = 300;

    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private SessionStore store;
    private SessionAdapter adapter;
    private TextView tvEmpty;
    private Button tabChat;
    private Button tabGuide;
    private String currentTab = "chat";   // chat=对话；guide=导游

    // 选导览方案 PDF（GetContent 免权限，返回 content:// 临时读授权）
    private final ActivityResultLauncher<String> pickPdf = registerForActivityResult(
            new ActivityResultContracts.GetContent(),
            uri -> {
                if (uri != null) importGuidePdf(uri);
            });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        store = new SessionStore(getFilesDir());
        tvEmpty = findViewById(R.id.tvEmpty);
        tabChat = findViewById(R.id.tabChat);
        tabGuide = findViewById(R.id.tabGuide);
        adapter = new SessionAdapter(
                s -> {
                    Intent it = new Intent(MainActivity.this,
                            s.type.equals("guide") ? GuideActivity.class : ChatActivity.class);
                    it.putExtra(ChatActivity.EXTRA_SESSION_ID, s.id);
                    it.putExtra(ChatActivity.EXTRA_SESSION_NAME, s.name);
                    startActivity(it);
                },
                s -> {
                    store.deleteSession(s.id);
                    refresh();
                });
        RecyclerView rv = findViewById(R.id.rvSessions);
        rv.setLayoutManager(new LinearLayoutManager(this));
        rv.setAdapter(adapter);
        findViewById(R.id.btnNew).setOnClickListener(v -> onNewClick());
        tabChat.setOnClickListener(v -> switchTab("chat"));
        tabGuide.setOnClickListener(v -> switchTab("guide"));
        switchTab("chat");
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private void switchTab(String tab) {
        currentTab = tab;
        int sel = 0xFFDCE3FF;
        int unsel = 0xFFFFFFFF;
        tabChat.setBackgroundColor(tab.equals("chat") ? sel : unsel);
        tabGuide.setBackgroundColor(tab.equals("guide") ? sel : unsel);
        ((Button) findViewById(R.id.btnNew)).setText(
                getString(tab.equals("chat") ? R.string.new_session : R.string.new_guide));
        refresh();
    }

    private void onNewClick() {
        if (currentTab.equals("chat")) {
            store.createSession("会话 " + (store.listSessions("chat").size() + 1), "chat", null);
            refresh();
        } else {
            pickPdf.launch("application/pdf");
        }
    }

    // importGuidePdf：复制 PDF 到 session 目录 → GuidePdf 解析（文本/图片）→
    //   方案文本切块灌入 AME 记忆图（截断外细节可被 recall 取证）→ 快照落盘。
    //   全程后台线程；每块一条事件，块号入 dia_id。
    private void importGuidePdf(Uri uri) {
        io.execute(() -> {
            try {
                // lastPathSegment + substringAfterLast('/'/':') + removeSuffix(".pdf") 链式等价
                String seg = uri.getLastPathSegment();
                if (seg == null) seg = "导览方案";
                seg = substringAfterLast(substringAfterLast(seg, '/'), ':');
                if (seg.endsWith(".pdf")) seg = seg.substring(0, seg.length() - 4);
                if (seg.trim().isEmpty()) seg = "导览方案";   // ifBlank
                final String name = seg;
                Log.i(MemoryOrchestrator.TAG, "导入导览方案: " + name + " (" + uri + ")");
                Session s = store.createSession(name, "guide", "guide.pdf");
                File dir = store.sessionDir(s.id);
                try (InputStream input = getContentResolver().openInputStream(uri)) {
                    if (input == null) throw new RuntimeException("无法读取所选文件");
                    Files.write(new File(dir, "guide.pdf").toPath(), readAllBytes(input));
                }
                GuidePdf.Plan plan = GuidePdf.parse(this, new File(dir, "guide.pdf"), dir);
                Log.i(MemoryOrchestrator.TAG, "PDF 解析完成: 文本" + plan.text.length()
                        + "字 图片" + plan.images.size() + "张");
                // use{} → try-with-resources（快照兜底落盘由 JNI close 负责）
                try (AmeEngine eng = AmeEngine.create(dir.getAbsolutePath())) {
                    long base = System.currentTimeMillis() / 1000;
                    // chunked(CHUNK).take(MAX) 等价：按块长切分并限制块数
                    List<String> chunks = new ArrayList<>();
                    for (int i = 0; i < plan.text.length() && chunks.size() < GUIDE_MAX_CHUNKS;
                         i += GUIDE_CHUNK_CHARS) {
                        chunks.add(plan.text.substring(i,
                                Math.min(i + GUIDE_CHUNK_CHARS, plan.text.length())));
                    }
                    for (int i = 0; i < chunks.size(); i++) {
                        eng.rememberTurn("guide", chunks.get(i), base + i, "g" + i, s.id);
                    }
                    eng.save();
                    Log.i(MemoryOrchestrator.TAG, "方案灌入记忆图: " + chunks.size() + "块");
                }
                runOnUiThread(() -> {
                    Toast.makeText(MainActivity.this,
                            "导览方案导入完成（" + plan.images.size() + " 张图）",
                            Toast.LENGTH_LONG).show();
                    refresh();
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    Toast.makeText(MainActivity.this, "方案导入失败: " + e.getMessage(),
                            Toast.LENGTH_LONG).show();
                    refresh();
                });
            }
        });
    }

    private void refresh() {
        List<Session> all = store.listSessions(currentTab);
        adapter.submit(all);
        tvEmpty.setVisibility(all.isEmpty() ? View.VISIBLE : View.GONE);
        tvEmpty.setText(getString(
                currentTab.equals("chat") ? R.string.empty_sessions : R.string.empty_guides));
    }

    @Override
    protected void onDestroy() {
        io.shutdownNow();
        super.onDestroy();
    }

    /** substringAfterLast 等价：找不到分隔符时返回原串 */
    private static String substringAfterLast(String s, char delim) {
        int i = s.lastIndexOf(delim);
        return i >= 0 ? s.substring(i + 1) : s;
    }

    /** input.readBytes() 等价 */
    private static byte[] readAllBytes(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
        return bos.toByteArray();
    }

    /** Kotlin lambda 参数的等价函数式接口 */
    interface SessionClick {
        void onClick(Session s);
    }

    // session 列表适配器：名称 + 创建时间（导游 session 加"导览"后缀）+ 删除按钮
    static class SessionAdapter extends RecyclerView.Adapter<SessionAdapter.VH> {

        private List<Session> data = new ArrayList<>();
        private final SimpleDateFormat fmt = new SimpleDateFormat("MM-dd HH:mm", Locale.US);
        private final SessionClick onClick;
        private final SessionClick onDelete;

        SessionAdapter(SessionClick onClick, SessionClick onDelete) {
            this.onClick = onClick;
            this.onDelete = onDelete;
        }

        void submit(List<Session> d) {
            data = d;
            notifyDataSetChanged();
        }

        static class VH extends RecyclerView.ViewHolder {
            final TextView name;
            final TextView meta;
            final Button del;

            VH(View v) {
                super(v);
                name = v.findViewById(R.id.tvName);
                meta = v.findViewById(R.id.tvMeta);
                del = v.findViewById(R.id.btnDelete);
            }
        }

        @Override
        public VH onCreateViewHolder(ViewGroup p, int vt) {
            return new VH(LayoutInflater.from(p.getContext())
                    .inflate(R.layout.item_session, p, false));
        }

        @Override
        public int getItemCount() {
            return data.size();
        }

        @Override
        public void onBindViewHolder(VH h, int pos) {
            Session s = data.get(pos);
            h.name.setText(s.name);
            h.meta.setText(fmt.format(new Date(s.createdAt))
                    + (s.type.equals("guide") ? " · 导览方案" : ""));
            h.itemView.setOnClickListener(v -> onClick.onClick(s));
            h.del.setOnClickListener(v -> onDelete.onClick(s));
        }
    }
}
