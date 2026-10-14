/*
 * 所属模块：app——聊天页。
 * 主体思路：顶部状态栏（上下文外置可视化：累计记忆 token vs 实际发送 token 对比）
 *   + 消息列表（图片轮显缩略图）+ 输入栏（选图/拍照/发送）；
 *   发送后在后台线程跑 MemoryOrchestrator.chat（记忆写入→装配→MiniMax→落库），
 *   回主线程刷新列表与状态栏；引擎实例随 Activity 生命周期创建/关闭（关闭时
 *   自动快照落盘）。
 * 关键口径：token 估算 CJK 1 字≈1 token、ASCII 4 字符≈1 token、图片 1500 token/张
 *   （MemoryOrchestrator.estimateTokens）；累计记忆 token = 引擎事件总字符数 / 2
 *   （中英混合均值口径，仅用于对比展示）。
 * 依赖关系：AmeEngine / MemoryOrchestrator / SessionStore；Activity Result API
 *   （PickVisualMedia 免权限；TakePicture 走 FileProvider，不声明 CAMERA 权限）。
 */
package com.ame.memory;

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
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class ChatActivity extends AppCompatActivity {

    public static final String EXTRA_SESSION_ID = "session_id";
    public static final String EXTRA_SESSION_NAME = "session_name";

    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private MemEngine engine;
    private MemoryOrchestrator orchestrator;
    private SessionStore store;
    private String sessionId;
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
    private volatile boolean ready = false;  // 引擎+词典异步初始化完成标志

    // 选图（系统相册 picker，免运行时权限）
    private final ActivityResultLauncher<PickVisualMediaRequest> pickImage =
            registerForActivityResult(new ActivityResultContracts.PickVisualMedia(),
                    uri -> {
                        if (uri != null) stageImage(uri);
                    });
    // 拍照（系统相机 App 写入 FileProvider 提供的 Uri）
    private final ActivityResultLauncher<Uri> takePicture =
            registerForActivityResult(new ActivityResultContracts.TakePicture(),
                    ok -> {
                        if (ok && cameraOut != null) stageImage(cameraOut);
                    });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_chat);
        sessionId = getIntent().getStringExtra(EXTRA_SESSION_ID);
        if (sessionId == null) {
            finish();
            return;
        }
        setTitle(getIntent().getStringExtra(EXTRA_SESSION_NAME) != null
                ? getIntent().getStringExtra(EXTRA_SESSION_NAME) : "会话");

        store = new SessionStore(getFilesDir());
        // 蓝方修复：引擎创建（加载 graph.json 快照）+ 词典载入移出 UI 线程——
        // 大图快照同步解析曾导致进页面卡死；就绪前发送/诊断 gated
        io.execute(() -> {
            // 双后端：MEM_STORE=lmem 走 2.0 分层记忆（自动迁移 1.x 快照），默认 graph
            String dir = store.sessionDir(sessionId).getAbsolutePath();
            MemEngine eng = "lmem".equals(BuildConfig.MEM_STORE)
                    ? LmemEngine.create(dir, sessionId)
                    : AmeEngine.create(dir);
            if (eng instanceof AmeEngine) KeywordDict.ensureLoaded(this, (AmeEngine) eng);
            runOnUiThread(() -> {
                engine = eng;
                orchestrator = new MemoryOrchestrator(eng, store, sessionId);
                diagAdapter = new DiagAdapter(orchestrator.diag());
                ((RecyclerView) findViewById(R.id.rvDiag)).setAdapter(diagAdapter);
                ready = true;
                refreshStatus();
            });
        });

        tvStatus = findViewById(R.id.tvStatus);
        msgs.addAll(store.loadMessages(sessionId));
        adapter = new MsgAdapter(msgs);
        RecyclerView rv = findViewById(R.id.rvMessages);
        LinearLayoutManager lm = new LinearLayoutManager(this);
        lm.setStackFromEnd(true);
        rv.setLayoutManager(lm);
        rv.setAdapter(adapter);
        // 诊断 tab：展示每轮提交给 LLM 的完整上下文（system+证据+滑窗）；
        // 适配器在引擎就绪回调里赋值（见上），这里先建列表
        RecyclerView rvDiag = findViewById(R.id.rvDiag);
        rvDiag.setLayoutManager(new LinearLayoutManager(ChatActivity.this));

        EditText et = findViewById(R.id.etInput);
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
                    String reply = orchestrator.chat(text, imgB64, null, webOn);
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
                        Toast.makeText(ChatActivity.this, "请求失败: " + e.getMessage(),
                                Toast.LENGTH_LONG).show();
                        refreshStatus();
                    });
                }
            });
        });
    }

    // stageImage 伪代码：解码 bounds → 按最长边 1024 计算 inSampleSize 粗采样 →
    //   精确缩放 ≤1024 → JPEG 80 压缩 → **仅内存 base64 入待发状态（不落盘）** →
    //   预览行显示缩略图。
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

    // appendLocal 伪代码：追加到内存列表并滚动到底（消息已在上游 jsonl 落盘，此处只刷 UI）
    private void appendLocal(Message m) {
        msgs.add(m);
        adapter.notifyItemInserted(msgs.size() - 1);
        ((RecyclerView) findViewById(R.id.rvMessages)).scrollToPosition(msgs.size() - 1);
    }

    // refreshStatus 伪代码：引擎统计（累计轮数/字符数/节点数）+ 上轮发送 token →
    //   状态栏对比行："记忆 N轮 ~Xtok │ 发送 Ytok │ 压缩 Z× │ 节点 M │ 窗口z+证据k"。
    private void refreshStatus() {
        // 蓝方修复：页面销毁后 io 已 shutdown，网络回调晚到再 execute 会
        // RejectedExecutionException 崩主线程——销毁后直接跳过状态栏刷新；
        // 引擎异步初始化完成前 orchestrator 未建，同样跳过
        if (io.isShutdown() || !ready) return;
        io.execute(() -> {
            try {
                JSONObject s = new JSONObject(orchestrator.statsJson());
                long turns = s.optLong("turns");
                long cumTok = s.optLong("total_chars") / 2;  // 中英混合均值口径（chars/2）
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
        Log.d(MemoryOrchestrator.TAG, "[" + sessionId + "] 聊天页关闭，引擎快照落盘");
        io.shutdownNow();
        if (engine != null) engine.close();  // 释放原生引擎（JNI 侧兜底快照落盘）
        engine = null;
        super.onDestroy();
    }

    /** take(n) 等价：超过 n 字符截断，否则原样返回 */
    private static String take(String s, int n) {
        return s.length() > n ? s.substring(0, n) : s;
    }

    // 消息适配器：按角色区分对齐与底色（user 右/蓝，assistant 左/灰）；
    // 图片轮在文本上方显缩略图（jsonl 持久化的本地路径直接解码，图已 ≤1024 压缩）
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
