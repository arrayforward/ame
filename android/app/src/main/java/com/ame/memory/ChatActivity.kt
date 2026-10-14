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
package com.ame.memory

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.util.Base64
import android.view.Gravity
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.FileProvider
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.concurrent.Executors

class ChatActivity : AppCompatActivity() {

    companion object {
        const val EXTRA_SESSION_ID = "session_id"
        const val EXTRA_SESSION_NAME = "session_name"
    }

    private val io = Executors.newSingleThreadExecutor()
    private var engine: AmeEngine? = null
    private lateinit var orchestrator: MemoryOrchestrator
    private lateinit var store: SessionStore
    private lateinit var sessionId: String
    private lateinit var adapter: MsgAdapter
    private lateinit var tvStatus: TextView
    private val msgs = mutableListOf<Message>()

    // 待发送图片（压缩后文件 + base64）；null = 纯文本轮
    private var pendingImage: Pair<File, String>? = null
    private var cameraOut: Uri? = null

    // 选图（系统相册 picker，免运行时权限）
    private val pickImage =
        registerForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
            uri?.let { stageImage(it) }
        }
    // 拍照（系统相机 App 写入 FileProvider 提供的 Uri）
    private val takePicture =
        registerForActivityResult(ActivityResultContracts.TakePicture()) { ok ->
            if (ok) cameraOut?.let { stageImage(it) }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_chat)
        sessionId = intent.getStringExtra(EXTRA_SESSION_ID) ?: run { finish(); return }
        title = intent.getStringExtra(EXTRA_SESSION_NAME) ?: "会话"

        store = SessionStore(filesDir)
        val eng = AmeEngine.create(store.sessionDir(sessionId).absolutePath)
        engine = eng
        orchestrator = MemoryOrchestrator(eng, store, sessionId)

        tvStatus = findViewById(R.id.tvStatus)
        msgs.addAll(store.loadMessages(sessionId))
        adapter = MsgAdapter(msgs)
        val rv = findViewById<RecyclerView>(R.id.rvMessages)
        rv.layoutManager = LinearLayoutManager(this).apply { stackFromEnd = true }
        rv.adapter = adapter

        val et = findViewById<EditText>(R.id.etInput)
        findViewById<Button>(R.id.btnPick).setOnClickListener {
            pickImage.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
        }
        findViewById<Button>(R.id.btnCamera).setOnClickListener {
            val f = File(File(filesDir, "images").apply { mkdirs() },
                         "cam_${System.currentTimeMillis()}.jpg")
            val uri = FileProvider.getUriForFile(this, "com.ame.memory.fileprovider", f)
            cameraOut = uri
            takePicture.launch(uri)
        }
        findViewById<Button>(R.id.btnClearImg).setOnClickListener { clearPending() }
        findViewById<Button>(R.id.btnSend).setOnClickListener {
            val text = et.text.toString().trim()
            val img = pendingImage
            if (text.isEmpty() && img == null) return@setOnClickListener
            et.text.clear()
            clearPending()
            appendLocal(Message("user", text.ifBlank { "[图片]" },
                                System.currentTimeMillis() / 1000, img?.first?.absolutePath))
            io.execute {
                try {
                    val reply = orchestrator.chat(text, img?.second, img?.first?.absolutePath)
                    runOnUiThread {
                        appendLocal(Message("assistant", reply, 0))
                        refreshStatus()
                    }
                } catch (e: Exception) {
                    runOnUiThread {
                        Toast.makeText(this, "请求失败: ${e.message}", Toast.LENGTH_LONG).show()
                        refreshStatus()
                    }
                }
            }
        }
        refreshStatus()
    }

    // stageImage 伪代码：解码 bounds → 按最长边 1024 计算 inSampleSize 粗采样 →
    //   精确缩放 ≤1024 → JPEG 80 压缩 → 存 session images 目录 + base64 入待发状态 →
    //   预览行显示缩略图。
    private fun stageImage(uri: Uri) {
        try {
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            contentResolver.openInputStream(uri)?.use {
                BitmapFactory.decodeStream(it, null, bounds)
            }
            if (bounds.outWidth <= 0) throw RuntimeException("无法解码图片")
            var sample = 1
            while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= 1024) sample *= 2
            val opts = BitmapFactory.Options().apply { inSampleSize = sample }
            val raw = contentResolver.openInputStream(uri)?.use {
                BitmapFactory.decodeStream(it, null, opts)
            } ?: throw RuntimeException("无法解码图片")
            val scale = 1024f / maxOf(raw.width, raw.height)
            val bmp = if (scale < 1f)
                Bitmap.createScaledBitmap(raw, (raw.width * scale).toInt(),
                                          (raw.height * scale).toInt(), true)
            else raw
            val bos = ByteArrayOutputStream()
            bmp.compress(Bitmap.CompressFormat.JPEG, 80, bos)
            val bytes = bos.toByteArray()
            val f = File(File(filesDir, "images").apply { mkdirs() },
                         "img_${System.currentTimeMillis()}.jpg")
            f.writeBytes(bytes)
            pendingImage = f to Base64.encodeToString(bytes, Base64.NO_WRAP)
            findViewById<ImageView>(R.id.ivPending).setImageBitmap(bmp)
            findViewById<LinearLayout>(R.id.rowPending).visibility = View.VISIBLE
        } catch (e: Exception) {
            Toast.makeText(this, "图片处理失败: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }

    private fun clearPending() {
        pendingImage = null
        findViewById<LinearLayout>(R.id.rowPending).visibility = View.GONE
    }

    // appendLocal 伪代码：追加到内存列表并滚动到底（消息已在上游 jsonl 落盘，此处只刷 UI）
    private fun appendLocal(m: Message) {
        msgs.add(m)
        adapter.notifyItemInserted(msgs.size - 1)
        findViewById<RecyclerView>(R.id.rvMessages).scrollToPosition(msgs.size - 1)
    }

    // refreshStatus 伪代码：引擎统计（累计轮数/字符数/节点数）+ 上轮发送 token →
    //   状态栏对比行："记忆 N轮 ~Xtok │ 发送 Ytok │ 压缩 Z× │ 节点 M │ 窗口z+证据k"。
    private fun refreshStatus() {
        io.execute {
            val s = JSONObject(orchestrator.statsJson())
            val turns = s.optLong("turns")
            val cumTok = s.optLong("total_chars") / 2  // 中英混合均值口径（chars/2）
            val nodes = s.optLong("nodes")
            val sent = orchestrator.lastSentTokens
            val ratio = if (sent > 0 && cumTok > 0)
                String.format("%.1f×", cumTok.toDouble() / sent) else "—"
            val line = "记忆 ${turns}轮 ~${cumTok}tok │ 发送 ${sent}tok │ 压缩 $ratio │ " +
                    "节点 $nodes │ 窗口${BuildConfig.CONTEXT_WINDOW_TURNS}+" +
                    "证据${BuildConfig.RECALL_TOP_K}"
            runOnUiThread { tvStatus.text = line }
        }
    }

    override fun onDestroy() {
        io.shutdownNow()
        engine?.close()  // 释放原生引擎（JNI 侧兜底快照落盘）
        engine = null
        super.onDestroy()
    }

    // 消息适配器：按角色区分对齐与底色（user 右/蓝，assistant 左/灰）；
    // 图片轮在文本上方显缩略图（jsonl 持久化的本地路径直接解码，图已 ≤1024 压缩）
    class MsgAdapter(private val data: List<Message>) :
        RecyclerView.Adapter<MsgAdapter.VH>() {

        class VH(v: View) : RecyclerView.ViewHolder(v) {
            val root: LinearLayout = v.findViewById(R.id.msgRoot)
            val tv: TextView = v.findViewById(R.id.tvMsg)
            val iv: ImageView = v.findViewById(R.id.ivMsg)
        }

        override fun onCreateViewHolder(p: ViewGroup, vt: Int) = VH(
            LayoutInflater.from(p.context).inflate(R.layout.item_message, p, false)
        )

        override fun getItemCount() = data.size

        override fun onBindViewHolder(h: VH, pos: Int) {
            val m = data[pos]
            h.tv.text = m.text
            if (m.imagePath != null) {
                h.iv.setImageBitmap(BitmapFactory.decodeFile(m.imagePath))
                h.iv.visibility = View.VISIBLE
            } else {
                h.iv.visibility = View.GONE
            }
            if (m.role == "user") {
                h.tv.gravity = Gravity.END
                h.root.gravity = Gravity.END
                h.tv.setBackgroundColor(Color.parseColor("#DCE3FF"))
            } else {
                h.tv.gravity = Gravity.START
                h.root.gravity = Gravity.START
                h.tv.setBackgroundColor(Color.parseColor("#F2F2F2"))
            }
        }
    }
}
