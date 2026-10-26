/*
 * 所属模块：app——AME 引擎 JNI 声明类（1.x graph 后端）。
 * 主体思路：静态代码块加载 libame_jni.so；全部 native 方法以 private static
 *   声明在本类中（JNI 符号名为 Java_com_ame_memory_AmeEngine_*），
 *   实例方法持句柄做薄壳封装，close() 释放原生引擎（含快照兜底落盘）。
 * 依赖关系：libame_jni（CMake 外源构建，见 ame_jni/CMakeLists.txt）；
 *   3.0 简化版：唯一存储后端（2.0 lmem 已裁剪，dev-3.0 刀1）。
 */
package com.ame.memory;

public final class AmeEngine implements AutoCloseable {

    static {
        System.loadLibrary("ame_jni");
    }

    private final long handle;
    private final String dir;

    private AmeEngine(long handle, String dir) {
        this.handle = handle;
        this.dir = dir;
    }

    public static AmeEngine create(String dir) {
        return new AmeEngine(nativeCreateSession(dir), dir);
    }

    private static native long nativeCreateSession(String dir);

    private static native void nativeDestroySession(long handle, String dir);

    private static native int nativeRememberTurn(
            long handle, String speaker, String text, long ts, String diaId,
            String sessionId);

    private static native String nativeRecall(long handle, String query, int limit);

    private static native String nativeGetProfile(long handle);

    private static native String nativeStats(long handle);

    private static native int nativeSave(long handle, String dir);

    private static native int nativeLoad(long handle, String dir);

    private static native int nativeLoadKeywordDict(long handle, String dict, String cache);

    private static native int nativeLoadCheapModel(long handle, String modelJson);

    public boolean isValid() {
        return handle != 0L;
    }

    /** 写入一轮对话（speaker=user/assistant；ts 为 epoch 秒） */
    public int rememberTurn(String speaker, String text, long ts, String diaId, String sessionId) {
        return nativeRememberTurn(handle, speaker, text, ts, diaId, sessionId);
    }

    /** 检索记忆证据，返回 JSON 数组 [{text,ts,score}] */
    public String recall(String query, int limit) {
        return nativeRecall(handle, query, limit);
    }

    /** 用户档案槽位 JSON {name,slots:[{slot,value,valid_from}]} */
    public String getProfile() {
        return nativeGetProfile(handle);
    }

    /** 引擎统计 JSON {turns,total_chars,events,nodes}（状态栏数据通路） */
    public String stats() {
        return nativeStats(handle);
    }

    public int save() {
        return nativeSave(handle, dir);
    }

    /** 加载中文关键词词典（AC 自动机 + 二进制缓存）；返回条数，<0 失败 */
    public int loadKeywordDict(String dictPath, String cachePath) {
        return nativeLoadKeywordDict(handle, dictPath, cachePath);
    }

    /** 注入 cheap 重排模型 JSON；1=启用（recall 走池60→特征重排→topN），0=解析失败回退原序 */
    public int loadCheapModel(String modelJson) {
        return nativeLoadCheapModel(handle, modelJson);
    }

    @Override
    public void close() {
        nativeDestroySession(handle, dir);
    }
}
