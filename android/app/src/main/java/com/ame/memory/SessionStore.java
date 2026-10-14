/*
 * 所属模块：app——session 元数据与消息记录的本地持久化。
 * 主体思路：filesDir/sessions.json 存 session 列表（id/name/createdAt）；
 *   每个 session 的消息流存 filesDir/sessions/<id>/messages.jsonl（追加写，一轮一行），
 *   AME 图快照存同目录 graph.json（由 JNI 侧负责）。
 * 关键数据结构：Session(id,name,createdAt,type,pdf)；Message(role,text,ts,imagePath)。
 *   （Java 版作为同包顶层类置于本文件，等价于 Kotlin 的顶层 data class。）
 * 依赖关系：org.json；java.io。
 * 注：org.json 的 JSONException 在 Java 中是受检异常，各方法统一包装为
 *   RuntimeException 外抛，与 Kotlin 的非受检传播语义一致。
 */
package com.ame.memory;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/** 一个会话：type: chat=普通对话；guide=导游（绑定导览方案 PDF）；
 * pdf 为 guide 模式的方案文件名（存 session 目录下），null 表示无 */
final class Session {
    public final String id;
    public final String name;
    public final long createdAt;
    public final String type;
    public final String pdf;

    Session(String id, String name, long createdAt, String type, String pdf) {
        this.id = id;
        this.name = name;
        this.createdAt = createdAt;
        this.type = type;
        this.pdf = pdf;
    }
}

/** 一条消息：role=user/assistant；imagePath 非空表示该轮带图（本地文件路径） */
final class Message {
    public final String role;
    public final String text;
    public final long ts;
    public final String imagePath;  // null = 纯文本轮

    Message(String role, String text, long ts, String imagePath) {
        this.role = role;
        this.text = text;
        this.ts = ts;
        this.imagePath = imagePath;
    }
}

public class SessionStore {

    private final File filesDir;

    public SessionStore(File filesDir) {
        this.filesDir = filesDir;
    }

    private File listFile() {
        return new File(filesDir, "sessions.json");
    }

    public File sessionDir(String id) {
        File d = new File(filesDir, "sessions/" + id);
        d.mkdirs();
        return d;
    }

    private File msgFile(String id) {
        return new File(sessionDir(id), "messages.jsonl");
    }

    public List<Session> listSessions() {
        File f = listFile();
        if (!f.exists()) return new ArrayList<>();
        try {
            JSONArray arr = new JSONArray(readText(f));
            List<Session> out = new ArrayList<>();
            for (int i = 0; i < arr.length(); i++) {
                JSONObject o = arr.getJSONObject(i);
                out.add(new Session(o.getString("id"), o.getString("name"),
                        o.getLong("createdAt"),
                        o.optString("type", "chat"),
                        o.has("pdf") ? o.getString("pdf") : null));
            }
            // sortedByDescending { createdAt }
            out.sort(Comparator.comparingLong((Session s) -> s.createdAt).reversed());
            return out;
        } catch (IOException | JSONException e) {
            throw new RuntimeException(e);
        }
    }

    /** 按类型过滤：tab 页各自只看自己的 session（导游/对话记忆互不串） */
    public List<Session> listSessions(String type) {
        List<Session> out = new ArrayList<>();
        for (Session s : listSessions()) {
            if (s.type.equals(type)) out.add(s);
        }
        return out;
    }

    public Session createSession(String name) {
        return createSession(name, "chat", null);
    }

    public Session createSession(String name, String type, String pdf) {
        try {
            // toString(36)：毫秒时间戳的 36 进制串作为唯一 id
            Session s = new Session(Long.toString(System.currentTimeMillis(), 36), name,
                    System.currentTimeMillis(), type, pdf);
            List<Session> all = new ArrayList<>(listSessions());
            all.add(s);
            JSONArray arr = new JSONArray();
            for (Session it : all) {
                JSONObject o = new JSONObject().put("id", it.id).put("name", it.name)
                        .put("createdAt", it.createdAt).put("type", it.type);
                if (it.pdf != null) o.put("pdf", it.pdf);
                arr.put(o);
            }
            writeText(listFile(), arr.toString());
            sessionDir(s.id);
            return s;
        } catch (JSONException e) {
            throw new RuntimeException(e);
        }
    }

    /** 删除 session：元数据 + 目录（消息/快照/导览方案及图片）一并移除 */
    public void deleteSession(String id) {
        try {
            JSONArray arr = new JSONArray();
            for (Session it : listSessions()) {
                if (it.id.equals(id)) continue;
                JSONObject o = new JSONObject().put("id", it.id).put("name", it.name)
                        .put("createdAt", it.createdAt).put("type", it.type);
                if (it.pdf != null) o.put("pdf", it.pdf);
                arr.put(o);
            }
            writeText(listFile(), arr.toString());
            deleteRecursively(sessionDir(id));
        } catch (JSONException e) {
            throw new RuntimeException(e);
        }
    }

    public void appendMessage(String sessionId, Message m) {
        JSONObject o;
        try {
            o = new JSONObject().put("role", m.role).put("text", m.text).put("ts", m.ts);
            if (m.imagePath != null) o.put("image", m.imagePath);
        } catch (JSONException e) {
            throw new RuntimeException(e);
        }
        File f = msgFile(sessionId);
        try (Writer w = Files.newBufferedWriter(
                f.toPath(), StandardCharsets.UTF_8,
                StandardOpenOption.CREATE,
                StandardOpenOption.APPEND)) {
            w.write(o.toString() + "\n");
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
    }

    public List<Message> loadMessages(String sessionId) {
        File f = msgFile(sessionId);
        List<Message> out = new ArrayList<>();
        if (!f.exists()) return out;
        try (BufferedReader br = new BufferedReader(new InputStreamReader(
                new FileInputStream(f), StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                if (line.trim().isEmpty()) continue;   // isNotBlank 过滤空行
                JSONObject o = new JSONObject(line);
                out.add(new Message(o.getString("role"), o.getString("text"), o.getLong("ts"),
                        o.has("image") ? o.getString("image") : null));
            }
            return out;
        } catch (IOException | JSONException e) {
            throw new RuntimeException(e);
        }
    }

    /** readText 等价：UTF-8 读整个文件 */
    private static String readText(File f) throws IOException {
        return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
    }

    /** writeText 等价：UTF-8 写整个文件 */
    private static void writeText(File f, String s) {
        try {
            Files.write(f.toPath(), s.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
    }

    /** deleteRecursively 等价：先删子文件/子目录，再删自身 */
    private static void deleteRecursively(File f) {
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) deleteRecursively(c);
        }
        f.delete();
    }
}
