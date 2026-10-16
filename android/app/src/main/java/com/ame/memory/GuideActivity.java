/*
 * 所属模块：app——导游聊天页。
 * 主体思路：与 ChatActivity 同构（状态栏 + 消息列表 + 选图/拍照/发送），差异：
 *   - 传入导览方案（guide.txt 全文注入 system prompt 前 8000 字，截断外细节靠
 *     导入时灌入记忆图的方案块经 recall 取证）；
 *   - 多一个「方案」按钮：弹窗查看方案全文 + 方案图片（PDF 内嵌图/页渲染图）；
 *   - 记忆机制不变：每 session 独立 Engine（每游客一个 session），拍照/问答双侧
 *     写入记忆图，删除 session 即清除该游客的全部记忆。
 * 关键口径：同 ChatActivity（CJK 1 字≈1 token、ASCII 4 字符≈1 token、图 1500 tok）。
 * 依赖关系：AmeEngine / MemoryOrchestrator / SessionStore / GuidePdf 产物目录。
 */
package com.ame.memory;

import android.app.AlertDialog;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.util.Base64;
import android.util.Log;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.PickVisualMediaRequest;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.content.FileProvider;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class GuideActivity extends AppCompatActivity {

    /** 方案弹窗最多展示的图片张数（防大图多占内存） */
    private static final int PLAN_DIALOG_MAX_IMGS = 20;

    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private MemEngine engine;
    private MemoryOrchestrator orchestrator;
    private SessionStore store;
    private String sessionId;
    private File sessionDir;
    private MsgAdapter adapter;
    private DiagAdapter diagAdapter;
    private boolean diagMode = false;   // false=消息列表；true=诊断列表（每轮提交上下文）
    private TextView tvStatus;
    private final List<Message> msgs = new ArrayList<>();

    // 待发送图片（仅内存 base64，不落盘——记忆只存 vision 摘要文本）；null = 纯文本轮
    private String pendingImageB64;
    private Uri cameraOut;
    private File cameraFile;   // 拍照临时落点（cacheDir，用完即删）
    private boolean webOn;   // 联网开关：开=本轮先 web_search 取证入记忆再作答
    private boolean deepOn;  // 深度重排开关（试跑档）：开=recall 全池 60 条 M3 语义重排取 top-5 注入
    private volatile boolean ready = false;  // 引擎+词典异步初始化完成标志

    private final ActivityResultLauncher<PickVisualMediaRequest> pickImage =
            registerForActivityResult(new ActivityResultContracts.PickVisualMedia(),
                    uri -> {
                        if (uri != null) stageImage(uri);
                    });
    private final ActivityResultLauncher<Uri> takePicture =
            registerForActivityResult(new ActivityResultContracts.TakePicture(),
                    ok -> {
                        if (ok && cameraOut != null) stageImage(cameraOut);
                    });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_guide);
        sessionId = getIntent().getStringExtra(ChatActivity.EXTRA_SESSION_ID);
        if (sessionId == null) {
            finish();
            return;
        }
        String sessionName = getIntent().getStringExtra(ChatActivity.EXTRA_SESSION_NAME);
        if (sessionName == null) sessionName = "导览";

        store = new SessionStore(getFilesDir());
        sessionDir = store.sessionDir(sessionId);
        // 蓝方修复：引擎创建（加载 graph.json 快照）+ 词典载入移出 UI 线程——
        // 大图快照同步解析曾导致进页面卡死；就绪前发送/诊断 gated
        // 导览方案：全文注入 prompt（截断由 MemoryOrchestrator 控制）；无方案文件则退化为普通记忆对话
        String guideText = null;
        File guideFile = new File(sessionDir, "guide.txt");
        if (guideFile.exists()) {
            try {
                guideText = new String(Files.readAllBytes(guideFile.toPath()),
                        StandardCharsets.UTF_8);
            } catch (IOException ignored) {
            }
        }
        if (guideText == null) {
            Toast.makeText(this, "未找到导览方案文件，按普通对话模式运行",
                    Toast.LENGTH_LONG).show();
        }
        final String guideTextF = guideText;
        final String sessionNameF = sessionName;   // lambda 捕获需 effectively final
        io.execute(() -> {
            // 双后端：MEM_STORE=lmem 走 2.0 分层记忆（自动迁移 1.x 快照），默认 graph
            MemEngine eng = "lmem".equals(BuildConfig.MEM_STORE)
                    ? LmemEngine.create(sessionDir.getAbsolutePath(), sessionId)
                    : AmeEngine.create(sessionDir.getAbsolutePath());
            if (eng instanceof AmeEngine) KeywordDict.ensureLoaded(this, (AmeEngine) eng);
            runOnUiThread(() -> {
                engine = eng;
                orchestrator = new MemoryOrchestrator(eng, store, sessionId,
                        BuildConfig.CONTEXT_WINDOW_TURNS, BuildConfig.RECALL_TOP_K,
                        sessionNameF, guideTextF);
                diagAdapter = new DiagAdapter(orchestrator.diag());
                ((RecyclerView) findViewById(R.id.rvDiag)).setAdapter(diagAdapter);
                ready = true;
                refreshStatus();
            });
        });

        setTitle("导游 · " + sessionName);
        tvStatus = findViewById(R.id.tvStatus);
        msgs.addAll(store.loadMessages(sessionId));
        adapter = new MsgAdapter(msgs);
        RecyclerView rv = findViewById(R.id.rvMessages);
        LinearLayoutManager lm = new LinearLayoutManager(this);
        lm.setStackFromEnd(true);
        rv.setLayoutManager(lm);
        rv.setAdapter(adapter);
        // 诊断 tab：展示每轮提交给 LLM 的完整上下文（方案+证据+滑窗）
        diagAdapter = new DiagAdapter(orchestrator.diag());
        RecyclerView rvDiag = findViewById(R.id.rvDiag);
        rvDiag.setLayoutManager(new LinearLayoutManager(GuideActivity.this));
        rvDiag.setAdapter(diagAdapter);

        EditText et = findViewById(R.id.etInput);
        final String planName = sessionName;   // lambda 引用需 final，等价 Kotlin 闭包捕获
        findViewById(R.id.btnPlan).setOnClickListener(v -> showPlanDialog(planName));
        findViewById(R.id.btnPick).setOnClickListener(v -> pickImage.launch(
                new PickVisualMediaRequest.Builder()
                        .setMediaType(ActivityResultContracts.PickVisualMedia.ImageOnly.INSTANCE)
                        .build()));
        findViewById(R.id.btnCamera).setOnClickListener(v -> {
            // 相机合约必须有落点：放 cacheDir 临时文件，stageImage 用完即删（不落盘原则）
            File f = new File(getCacheDir(), "cam_" + System.currentTimeMillis() + ".jpg");
            Uri uri = FileProvider.getUriForFile(this, "com.ame.memory.fileprovider", f);
            cameraFile = f;
            cameraOut = uri;
            takePicture.launch(uri);
        });
        findViewById(R.id.btnClearImg).setOnClickListener(v -> clearPending());
        // 联网开关（选中态变色提示）
        findViewById(R.id.btnWeb).setOnClickListener(v -> {
            webOn = !webOn;
            v.setBackgroundColor(webOn ? 0xFFDCE3FF : 0xFFFFFFFF);
        });
        // 深度重排开关（选中态变色，同联网开关模式；试跑档非默认）
        findViewById(R.id.btnDeep).setOnClickListener(v -> {
            deepOn = !deepOn;
            v.setBackgroundColor(deepOn ? 0xFFDCE3FF : 0xFFFFFFFF);
        });
        // 诊断/消息列表切换
        findViewById(R.id.btnDiag).setOnClickListener(v -> toggleDiag());
        findViewById(R.id.btnSend).setOnClickListener(v -> {
            if (!ready) {   // 引擎/词典异步初始化未完成
                Toast.makeText(this, "记忆加载中，请稍候…", Toast.LENGTH_SHORT).show();
                return;
            }
            String text = et.getText().toString().trim();
            String imgB64 = pendingImageB64;
            if (text.isEmpty() && imgB64 == null) return;   // return@setOnClickListener 等价
            et.getText().clear();
            clearPending();
            appendLocal(new Message("user", text.isEmpty() ? "[图片]" : text,   // ifBlank
                    System.currentTimeMillis() / 1000, null));
            Log.d(MemoryOrchestrator.TAG, "[" + sessionId + "] 发送: text="
                    + take(text, 60) + " 带图=" + (imgB64 != null));
            // 思考中占位（非流式作答的及时反馈），回复到达后原位替换
            final int pendingIdx = msgs.size();
            appendLocal(new Message("assistant", "思考中…", 0, null));
            io.execute(() -> {
                try {
                    String reply = orchestrator.chat(text, imgB64, null, webOn, deepOn);
                    runOnUiThread(() -> {
                        msgs.remove(pendingIdx);
                        adapter.notifyItemRemoved(pendingIdx);
                        appendLocal(new Message("assistant", reply, 0, null));
                        refreshStatus();
                    });
                } catch (Exception e) {
                    Log.e(MemoryOrchestrator.TAG, "[" + sessionId + "] 请求失败", e);
                    runOnUiThread(() -> {
                        msgs.remove(pendingIdx);
                        adapter.notifyItemRemoved(pendingIdx);
                        Toast.makeText(GuideActivity.this, "请求失败: " + e.getMessage(),
                                Toast.LENGTH_LONG).show();
                        refreshStatus();
                    });
                }
            });
        });
    }

    // showPlanDialog：ScrollView（方案全文 + 方案图片纵向排列）；图片上限 PLAN_DIALOG_MAX_IMGS 张。
    private void showPlanDialog(String sessionName) {
        ScrollView scroll = new ScrollView(this);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (12 * getResources().getDisplayMetrics().density);
        col.setPadding(pad, pad, pad, pad);
        String guide = null;
        File guideFile = new File(sessionDir, "guide.txt");
        if (guideFile.exists()) {
            try {
                guide = new String(Files.readAllBytes(guideFile.toPath()),
                        StandardCharsets.UTF_8);
            } catch (IOException ignored) {
            }
        }
        TextView tv = new TextView(this);
        tv.setText(guide != null ? guide : "（无方案文本）");   // ?.readText() ?: 等价
        tv.setTextSize(14f);
        col.addView(tv, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        List<File> plan = planImages();
        int limit = Math.min(PLAN_DIALOG_MAX_IMGS, plan.size());   // take(n) 等价
        for (int i = 0; i < limit; i++) {
            File f = plan.get(i);
            ImageView iv = new ImageView(this);
            iv.setImageBitmap(BitmapFactory.decodeFile(f.getAbsolutePath()));
            iv.setAdjustViewBounds(true);
            col.addView(iv, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        }
        scroll.addView(col);
        new AlertDialog.Builder(this)
                .setTitle("导览方案 · " + sessionName)
                .setView(scroll)
                .setPositiveButton("关闭", null)
                .show();
    }

    /** 方案图片：优先 PDF 内嵌图目录，其次页渲染图目录 */
    private List<File> planImages() {
        File imgs = new File(sessionDir, "guide_img");
        File pages = new File(sessionDir, "guide_page");
        // listFiles { extension == "jpg" }?.isNotEmpty() == true 等价
        File[] imgJpg = imgs.listFiles((dir, name) -> name.endsWith(".jpg"));
        boolean hasEmbedded = imgJpg != null && imgJpg.length > 0;
        File from = hasEmbedded ? imgs : pages;
        File[] jpg = from.listFiles((dir, name) -> name.endsWith(".jpg"));
        if (jpg == null) return new ArrayList<>();   // ?: emptyList()
        List<File> out = new ArrayList<>(Arrays.asList(jpg));
        out.sort(Comparator.comparing(f -> f.getName()));   // sortedBy { name }
        return out;
    }

    // toggleDiag：消息列表 ↔ 诊断列表切换；进入诊断时刷新快照列表。
    private void toggleDiag() {
        if (!ready) {
            Toast.makeText(this, "记忆加载中，请稍候…", Toast.LENGTH_SHORT).show();
            return;
        }
        diagMode = !diagMode;
        findViewById(R.id.rvMessages).setVisibility(
                diagMode ? View.GONE : View.VISIBLE);
        findViewById(R.id.rvDiag).setVisibility(
                diagMode ? View.VISIBLE : View.GONE);
        if (diagMode) {
            diagAdapter = new DiagAdapter(orchestrator.diag());
            ((RecyclerView) findViewById(R.id.rvDiag)).setAdapter(diagAdapter);
            if (orchestrator.diag().isEmpty()) {
                Toast.makeText(this, "本轮会话还没有对话记录", Toast.LENGTH_SHORT).show();
            }
        }
    }

    // stageImage：同 ChatActivity——最长边 1024、JPEG 80，存 images 目录 + base64 待发。
    private void stageImage(Uri uri) {
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                BitmapFactory.decodeStream(in, null, bounds);
            }
            if (bounds.outWidth <= 0) throw new RuntimeException("无法解码图片");
            int sample = 1;
            while (Math.max(bounds.outWidth, bounds.outHeight) / (sample * 2) >= 1024) {
                sample *= 2;
            }
            BitmapFactory.Options opts = new BitmapFactory.Options();
            opts.inSampleSize = sample;
            Bitmap raw;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                raw = BitmapFactory.decodeStream(in, null, opts);
            }
            if (raw == null) throw new RuntimeException("无法解码图片");
            float scale = 1024f / Math.max(raw.getWidth(), raw.getHeight());
            Bitmap bmp = scale < 1f
                    ? Bitmap.createScaledBitmap(raw, (int) (raw.getWidth() * scale),
                    (int) (raw.getHeight() * scale), true)
                    : raw;
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            bmp.compress(Bitmap.CompressFormat.JPEG, 80, bos);
            byte[] bytes = bos.toByteArray();
            pendingImageB64 = Base64.encodeToString(bytes, Base64.NO_WRAP);
            ((ImageView) findViewById(R.id.ivPending)).setImageBitmap(bmp);
            findViewById(R.id.rowPending).setVisibility(View.VISIBLE);
        } catch (Exception e) {
            Toast.makeText(this, "图片处理失败: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private void clearPending() {
        pendingImageB64 = null;
        // 拍照临时文件用完即删（图片不落盘原则的唯一例外：相机合约必须有落点）
        if (cameraFile != null) {
            if (cameraFile.exists()) cameraFile.delete();
            cameraFile = null;
        }
        findViewById(R.id.rowPending).setVisibility(View.GONE);
    }

    private void appendLocal(Message m) {
        msgs.add(m);
        adapter.notifyItemInserted(msgs.size() - 1);
        ((RecyclerView) findViewById(R.id.rvMessages)).scrollToPosition(msgs.size() - 1);
    }

    // refreshStatus：同 ChatActivity 口径（累计 chars/2 vs 发送估算）
    private void refreshStatus() {
        // 蓝方修复：页面销毁后 io 已 shutdown，网络回调晚到再 execute 会
        // RejectedExecutionException 崩主线程——销毁后直接跳过状态栏刷新
        if (io.isShutdown() || !ready) return;
        io.execute(() -> {
            try {
                JSONObject s = new JSONObject(orchestrator.statsJson());
                long turns = s.optLong("turns");
                long cumTok = s.optLong("total_chars") / 2;
                long nodes = s.optLong("nodes");
                int sent = orchestrator.getLastSentTokens();
                String ratio = (sent > 0 && cumTok > 0)
                        ? String.format("%.1f×", (double) cumTok / sent) : "—";
                String line = "记忆 " + turns + "轮 ~" + cumTok + "tok │ 发送 " + sent
                        + "tok │ 压缩 " + ratio + " │ 节点 " + nodes
                        + " │ 窗口" + BuildConfig.CONTEXT_WINDOW_TURNS
                        + "+证据" + BuildConfig.RECALL_TOP_K;
                runOnUiThread(() -> tvStatus.setText(line));
            } catch (Exception ignored) {
                // Kotlin 无受检异常，这里兜底：统计解析失败时状态栏保持原样
            }
        });
    }

    @Override
    protected void onDestroy() {
        Log.d(MemoryOrchestrator.TAG, "[" + sessionId + "] 导游页关闭，引擎快照落盘");
        io.shutdownNow();
        if (engine != null) engine.close();
        engine = null;
        super.onDestroy();
    }

    /** take(n) 等价：超过 n 字符截断，否则原样返回 */
    private static String take(String s, int n) {
        return s.length() > n ? s.substring(0, n) : s;
    }

    // 消息适配器：与 ChatActivity 同（user 右/蓝，assistant 左/灰，图片轮显缩略图）；
    // 图片路径持久化后重载可还原（orchestrator.chat 已写入 jsonl "image" 字段）。
    static class MsgAdapter extends RecyclerView.Adapter<MsgAdapter.VH> {

        private final List<Message> data;

        MsgAdapter(List<Message> data) {
            this.data = data;
        }

        static class VH extends RecyclerView.ViewHolder {
            final LinearLayout root;
            final TextView tv;
            final ImageView iv;

            VH(View v) {
                super(v);
                root = v.findViewById(R.id.msgRoot);
                tv = v.findViewById(R.id.tvMsg);
                iv = v.findViewById(R.id.ivMsg);
            }
        }

        @Override
        public VH onCreateViewHolder(ViewGroup p, int vt) {
            return new VH(LayoutInflater.from(p.getContext())
                    .inflate(R.layout.item_message, p, false));
        }

        @Override
        public int getItemCount() {
            return data.size();
        }

        @Override
        public void onBindViewHolder(VH h, int pos) {
            Message m = data.get(pos);
            h.tv.setText(m.text);
            // 图片不落盘：聊天图片只存 vision 摘要文本，消息列表无缩略图可显
            h.iv.setVisibility(View.GONE);
            if (m.role.equals("user")) {
                h.tv.setGravity(Gravity.END);
                h.root.setGravity(Gravity.END);
                h.tv.setBackgroundColor(Color.parseColor("#DCE3FF"));
            } else {
                h.tv.setGravity(Gravity.START);
                h.root.setGravity(Gravity.START);
                h.tv.setBackgroundColor(Color.parseColor("#F2F2F2"));
            }
        }
    }
}
