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
package com.ame.memory

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

class MainActivity : AppCompatActivity() {

    private val io = Executors.newSingleThreadExecutor()
    private lateinit var store: SessionStore
    private lateinit var adapter: SessionAdapter
    private lateinit var tvEmpty: TextView
    private lateinit var tabChat: Button
    private lateinit var tabGuide: Button
    private var currentTab = "chat"   // chat=对话；guide=导游

    // 选导览方案 PDF（GetContent 免权限，返回 content:// 临时读授权）
    private val pickPdf = registerForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        if (uri != null) importGuidePdf(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        store = SessionStore(filesDir)
        tvEmpty = findViewById(R.id.tvEmpty)
        tabChat = findViewById(R.id.tabChat)
        tabGuide = findViewById(R.id.tabGuide)
        adapter = SessionAdapter(
            onClick = { s ->
                val cls = if (s.type == "guide") GuideActivity::class.java
                          else ChatActivity::class.java
                startActivity(Intent(this, cls)
                    .putExtra(ChatActivity.EXTRA_SESSION_ID, s.id)
                    .putExtra(ChatActivity.EXTRA_SESSION_NAME, s.name))
            },
            onDelete = { s -> store.deleteSession(s.id); refresh() }
        )
        findViewById<RecyclerView>(R.id.rvSessions).apply {
            layoutManager = LinearLayoutManager(this@MainActivity)
            adapter = this@MainActivity.adapter
        }
        findViewById<Button>(R.id.btnNew).setOnClickListener { onNewClick() }
        tabChat.setOnClickListener { switchTab("chat") }
        tabGuide.setOnClickListener { switchTab("guide") }
        switchTab("chat")
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun switchTab(tab: String) {
        currentTab = tab
        val sel = 0xFFDCE3FF.toInt(); val unsel = 0xFFFFFFFF.toInt()
        tabChat.setBackgroundColor(if (tab == "chat") sel else unsel)
        tabGuide.setBackgroundColor(if (tab == "guide") sel else unsel)
        findViewById<Button>(R.id.btnNew).text =
            getString(if (tab == "chat") R.string.new_session else R.string.new_guide)
        refresh()
    }

    private fun onNewClick() {
        if (currentTab == "chat") {
            store.createSession("会话 " + (store.listSessions("chat").size + 1), "chat")
            refresh()
        } else {
            pickPdf.launch("application/pdf")
        }
    }

    // importGuidePdf：复制 PDF 到 session 目录 → GuidePdf 解析（文本/图片）→
    //   方案文本切块灌入 AME 记忆图（截断外细节可被 recall 取证）→ 快照落盘。
    //   全程后台线程；每块一条事件，块号入 dia_id。
    private fun importGuidePdf(uri: Uri) {
        io.execute {
            try {
                val name = (uri.lastPathSegment ?: "导览方案")
                    .substringAfterLast('/').substringAfterLast(':')
                    .removeSuffix(".pdf").ifBlank { "导览方案" }
                val s = store.createSession(name, "guide", "guide.pdf")
                val dir = store.sessionDir(s.id)
                contentResolver.openInputStream(uri)?.use { input ->
                    File(dir, "guide.pdf").writeBytes(input.readBytes())
                } ?: throw RuntimeException("无法读取所选文件")
                val plan = GuidePdf.parse(this, File(dir, "guide.pdf"), dir)
                AmeEngine.create(dir.absolutePath).use { eng ->
                    val base = System.currentTimeMillis() / 1000
                    plan.text.chunked(GUIDE_CHUNK_CHARS).take(GUIDE_MAX_CHUNKS)
                        .forEachIndexed { i, chunk ->
                            eng.rememberTurn("guide", chunk, base + i, "g$i", s.id)
                        }
                    eng.save()
                }
                runOnUiThread {
                    Toast.makeText(this,
                        "导览方案导入完成（${plan.images.size} 张图）",
                        Toast.LENGTH_LONG).show()
                    refresh()
                }
            } catch (e: Exception) {
                runOnUiThread {
                    Toast.makeText(this, "方案导入失败: ${e.message}",
                                   Toast.LENGTH_LONG).show()
                    refresh()
                }
            }
        }
    }

    private fun refresh() {
        val all = store.listSessions(currentTab)
        adapter.submit(all)
        tvEmpty.visibility = if (all.isEmpty()) View.VISIBLE else View.GONE
        tvEmpty.text = getString(
            if (currentTab == "chat") R.string.empty_sessions else R.string.empty_guides)
    }

    override fun onDestroy() {
        io.shutdownNow()
        super.onDestroy()
    }

    // session 列表适配器：名称 + 创建时间（导游 session 加"导览"后缀）+ 删除按钮
    class SessionAdapter(
        private val onClick: (Session) -> Unit,
        private val onDelete: (Session) -> Unit
    ) : RecyclerView.Adapter<SessionAdapter.VH>() {

        private var data: List<Session> = emptyList()
        private val fmt = SimpleDateFormat("MM-dd HH:mm", Locale.US)

        fun submit(d: List<Session>) {
            data = d
            notifyDataSetChanged()
        }

        class VH(v: View) : RecyclerView.ViewHolder(v) {
            val name: TextView = v.findViewById(R.id.tvName)
            val meta: TextView = v.findViewById(R.id.tvMeta)
            val del: Button = v.findViewById(R.id.btnDelete)
        }

        override fun onCreateViewHolder(p: ViewGroup, vt: Int) = VH(
            LayoutInflater.from(p.context).inflate(R.layout.item_session, p, false)
        )

        override fun getItemCount() = data.size

        override fun onBindViewHolder(h: VH, pos: Int) {
            val s = data[pos]
            h.name.text = s.name
            h.meta.text = fmt.format(Date(s.createdAt)) +
                    if (s.type == "guide") " · 导览方案" else ""
            h.itemView.setOnClickListener { onClick(s) }
            h.del.setOnClickListener { onDelete(s) }
        }
    }

    companion object {
        /** 方案切块大小（字符）：够 recall 命中片段即可，块越小图越细但事件越多 */
        private const val GUIDE_CHUNK_CHARS = 200
        /** 方案最多灌入记忆图的块数（超大 PDF 的护栏） */
        private const val GUIDE_MAX_CHUNKS = 300
    }
}
