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
package com.ame.memory

import android.app.AlertDialog
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
import android.widget.ScrollView
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

class GuideActivity : AppCompatActivity() {

    private val io = Executors.newSingleThreadExecutor()
    private var engine: AmeEngine? = null
    private lateinit var orchestrator: MemoryOrchestrator
    private lateinit var store: SessionStore
    private lateinit var sessionId: String
    private lateinit var sessionDir: File
    private lateinit var adapter: MsgAdapter
    private lateinit var tvStatus: TextView
    private val msgs = mutableListOf<Message>()

    // 待发送图片（压缩后文件 + base64）；null = 纯文本轮
    private var pendingImage: Pair<File, String>? = null
    private var cameraOut: Uri? = null

    private val pickImage =
        registerForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
            uri?.let { stageImage(it) }
        }
    private val takePicture =
        registerForActivityResult(ActivityResultContracts.TakePicture()) { ok ->
            if (ok) cameraOut?.let { stageImage(it) }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_guide)
        sessionId = intent.getStringExtra(ChatActivity.EXTRA_SESSION_ID) ?: run { finish(); return }
        val sessionName = intent.getStringExtra(ChatActivity.EXTRA_SESSION_NAME) ?: "导览"

        store = SessionStore(filesDir)
        sessionDir = store.sessionDir(sessionId)
        val eng = AmeEngine.create(sessionDir.absolutePath)
        engine = eng
        // 导览方案：全文注入 prompt（截断由 MemoryOrchestrator 控制）；无方案文件则退化为普通记忆对话
        val guideText = File(sessionDir, "guide.txt").takeIf { it.exists() }?.readText()
        if (guideText == null) {
            Toast.makeText(this, "未找到导览方案文件，按普通对话模式运行",
                           Toast.LENGTH_LONG).show()
        }
        orchestrator = MemoryOrchestrator(eng, store, sessionId,
                                          guideTitle = sessionName, guideText = guideText)

        title = "导游 · $sessionName"
        tvStatus = findViewById(R.id.tvStatus)
        msgs.addAll(store.loadMessages(sessionId))
        adapter = MsgAdapter(msgs)
        val rv = findViewById<RecyclerView>(R.id.rvMessages)
        rv.layoutManager = LinearLayoutManager(this).apply { stackFromEnd = true }
        rv.adapter = adapter

        val et = findViewById<EditText>(R.id.etInput)
        findViewById<Button>(R.id.btnPlan).setOnClickListener { showPlanDialog(sessionName) }
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

    // showPlanDialog：ScrollView（方案全文 + 方案图片纵向排列）；图片上限 PLAN_DIALOG_MAX_IMGS 张。
    private fun showPlanDialog(sessionName: String) {
        val scroll = ScrollView(this)
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val pad = (12 * resources.displayMetrics.density).toInt()
        col.setPadding(pad, pad, pad, pad)
        val tv = TextView(this).apply {
            text = File(sessionDir, "guide.txt").takeIf { it.exists() }
                ?.readText() ?: "（无方案文本）"
            textSize = 14f
        }
        col.addView(tv, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        planImages().take(PLAN_DIALOG_MAX_IMGS).forEach { f ->
            val iv = ImageView(this).apply {
                setImageBitmap(BitmapFactory.decodeFile(f.absolutePath))
                adjustViewBounds = true
            }
            col.addView(iv, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        }
        scroll.addView(col)
        AlertDialog.Builder(this)
            .setTitle("导览方案 · $sessionName")
            .setView(scroll)
            .setPositiveButton("关闭", null)
            .show()
    }

    /** 方案图片：优先 PDF 内嵌图目录，其次页渲染图目录 */
    private fun planImages(): List<File> {
        val imgs = File(sessionDir, "guide_img")
        val pages = File(sessionDir, "guide_page")
        val from = if (imgs.listFiles { f -> f.extension == "jpg" }?.isNotEmpty() == true)
            imgs else pages
        return from.listFiles { f -> f.extension == "jpg" }?.sortedBy { it.name }
            ?: emptyList()
    }

    // stageImage：同 ChatActivity——最长边 1024、JPEG 80，存 images 目录 + base64 待发。
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

    private fun appendLocal(m: Message) {
        msgs.add(m)
        adapter.notifyItemInserted(msgs.size - 1)
        findViewById<RecyclerView>(R.id.rvMessages).scrollToPosition(msgs.size - 1)
    }

    // refreshStatus：同 ChatActivity 口径（累计 chars/2 vs 发送估算）
    private fun refreshStatus() {
        io.execute {
            val s = JSONObject(orchestrator.statsJson())
            val turns = s.optLong("turns")
            val cumTok = s.optLong("total_chars") / 2
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
        engine?.close()
        engine = null
        super.onDestroy()
    }

    // 消息适配器：与 ChatActivity 同（user 右/蓝，assistant 左/灰，图片轮显缩略图）；
    // 图片路径持久化后重载可还原（orchestrator.chat 已写入 jsonl "image" 字段）。
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

    companion object {
        /** 方案弹窗最多展示的图片张数（防大图多占内存） */
        private const val PLAN_DIALOG_MAX_IMGS = 20
    }
}
