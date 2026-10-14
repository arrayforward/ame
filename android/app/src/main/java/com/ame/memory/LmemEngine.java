/*
 * 所属模块：app——AME 2.0 分层记忆（M15–M19）JNI 声明类（lmem 后端）。
 * 主体思路：与 AmeEngine 同形的薄壳——静态代码块加载 libame_jni.so；
 *   native 方法以 private static 声明（JNI 符号名 Java_com_ame_memory_LmemEngine_*）；
 *   create(sessionDir, owner=sessionId) 在 sessionDir/lmem/ 下建 Mem 网关，
 *   首次打开若发现 1.x graph.json 自动迁移进观测层（JNI 侧写 migrated.flag 防重复）。
 * 关键算法/数据结构：rememberTurn 的 speaker 映射 kind（user→user，其余→assistant），
 *   diaId/sessionId 语义不变（sessionId 即观测 channel）。
 * 依赖关系：libame_jni（mem_jni.cpp）；实现 MemEngine 接口。
 */
package com.ame.memory;

public final class LmemEngine implements MemEngine {

    static {
        System.loadLibrary("ame_jni");
    }

    private final long handle;

    private LmemEngine(long handle) {
        this.handle = handle;
    }

    /** 在 sessionDir/lmem/ 下建 2.0 分层记忆（owner=sessionId） */
    public static LmemEngine create(String sessionDir, String owner) {
        return new LmemEngine(nativeCreate(sessionDir + "/lmem", owner));
    }

    private static native long nativeCreate(String rootDir, String owner);

    private static native int nativeRemember(
            long handle, String channel, String kind, String text, long ts);

    private static native String nativeRecall(long handle, String query, int limit);

    private static native String nativeProfile(long handle);

    private static native String nativeStats(long handle);

    private static native int nativeEraseChannel(long handle, String channel);

    private static native int nativeSave(long handle);

    private static native void nativeClose(long handle);

    public boolean isValid() {
        return handle != 0L;
    }

    /** 删除该会话的记忆（观测+事实级联抹除） */
    public int eraseChannel(String channel) {
        return nativeEraseChannel(handle, channel);
    }

    /** 写入一轮对话（speaker=user/assistant；ts 为 epoch 秒） */
    @Override
    public int rememberTurn(String speaker, String text, long ts, String diaId, String sessionId) {
        String kind = "user".equals(speaker) ? "user" : "assistant";
        return nativeRemember(handle, sessionId, kind, text, ts);
    }

    /** 检索记忆证据，返回 JSON 数组 [{text,ts,score,dia_id}] */
    @Override
    public String recall(String query, int limit) {
        return nativeRecall(handle, query, limit);
    }

    /** 用户档案 JSON（fold 物化最新视图，entity→attr 嵌套） */
    @Override
    public String getProfile() {
        return nativeProfile(handle);
    }

    /** 引擎统计 JSON {turns,total_chars,events,nodes}（状态栏数据通路） */
    @Override
    public String stats() {
        return nativeStats(handle);
    }

    /** lmem 为逐条 append 落盘——no-op 返回 0（保留接口与 1.x 同形） */
    @Override
    public int save() {
        return nativeSave(handle);
    }

    @Override
    public void close() {
        nativeClose(handle);
    }
}
