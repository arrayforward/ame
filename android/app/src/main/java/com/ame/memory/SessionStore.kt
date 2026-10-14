/*
 * 所属模块：app——session 元数据与消息记录的本地持久化。
 * 主体思路：filesDir/sessions.json 存 session 列表（id/name/createdAt）；
 *   每个 session 的消息流存 filesDir/sessions/<id>/messages.jsonl（追加写，一轮一行），
 *   AME 图快照存同目录 graph.json（由 JNI 侧负责）。
 * 关键数据结构：Session(id,name,createdAt)；Message(role,text,ts)。
 * 依赖关系：org.json；java.io。
 */
package com.ame.memory

import org.json.JSONArray
import org.json.JSONObject
import java.io.File

data class Session(
    val id: String, val name: String, val createdAt: Long,
    val type: String = "chat",          // chat=普通对话；guide=导游（绑定导览方案 PDF）
    val pdf: String? = null             // guide 模式的方案文件名（存 session 目录下）
)

/** 一条消息：role=user/assistant；imagePath 非空表示该轮带图（本地文件路径） */
data class Message(val role: String, val text: String, val ts: Long, val imagePath: String? = null)

class SessionStore(private val filesDir: File) {

    private val listFile get() = File(filesDir, "sessions.json")
    fun sessionDir(id: String) = File(filesDir, "sessions/$id").apply { mkdirs() }
    private fun msgFile(id: String) = File(sessionDir(id), "messages.jsonl")

    fun listSessions(): List<Session> {
        if (!listFile.exists()) return emptyList()
        val arr = JSONArray(listFile.readText())
        return (0 until arr.length()).map {
            val o = arr.getJSONObject(it)
            Session(o.getString("id"), o.getString("name"), o.getLong("createdAt"),
                    o.optString("type", "chat"),
                    if (o.has("pdf")) o.getString("pdf") else null)
        }.sortedByDescending { it.createdAt }
    }

    /** 按类型过滤：tab 页各自只看自己的 session（导游/对话记忆互不串） */
    fun listSessions(type: String): List<Session> =
        listSessions().filter { it.type == type }

    fun createSession(name: String, type: String = "chat", pdf: String? = null): Session {
        val s = Session(System.currentTimeMillis().toString(36), name,
                        System.currentTimeMillis(), type, pdf)
        val all = listSessions().toMutableList().apply { add(s) }
        val arr = JSONArray()
        all.forEach {
            val o = JSONObject().put("id", it.id).put("name", it.name)
                .put("createdAt", it.createdAt).put("type", it.type)
            if (it.pdf != null) o.put("pdf", it.pdf)
            arr.put(o)
        }
        listFile.writeText(arr.toString())
        sessionDir(s.id)
        return s
    }

    /** 删除 session：元数据 + 目录（消息/快照/导览方案及图片）一并移除 */
    fun deleteSession(id: String) {
        val arr = JSONArray()
        listSessions().filter { it.id != id }.forEach {
            val o = JSONObject().put("id", it.id).put("name", it.name)
                .put("createdAt", it.createdAt).put("type", it.type)
            if (it.pdf != null) o.put("pdf", it.pdf)
            arr.put(o)
        }
        listFile.writeText(arr.toString())
        sessionDir(id).deleteRecursively()
    }

    fun appendMessage(sessionId: String, m: Message) {
        val o = JSONObject().put("role", m.role).put("text", m.text).put("ts", m.ts)
        if (m.imagePath != null) o.put("image", m.imagePath)
        msgFile(sessionId).appendText(o.toString() + "\n")
    }

    fun loadMessages(sessionId: String): List<Message> {
        val f = msgFile(sessionId)
        if (!f.exists()) return emptyList()
        return f.readLines().filter { it.isNotBlank() }.map {
            val o = JSONObject(it)
            Message(o.getString("role"), o.getString("text"), o.getLong("ts"),
                    if (o.has("image")) o.getString("image") else null)
        }
    }
}
