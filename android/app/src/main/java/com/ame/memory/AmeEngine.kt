/*
 * 所属模块：app——AME 引擎 JNI 声明类。
 * 主体思路：companion object 加载 libame_jni.so；全部 native 方法以 @JvmStatic
 *   external 声明在伴生对象中（JNI 符号名为 Java_com_ame_memory_AmeEngine_*），
 *   实例方法持句柄做薄壳封装，close() 释放原生引擎（含快照兜底落盘）。
 * 依赖关系：libame_jni（CMake 外源构建，见 ame_jni/CMakeLists.txt）。
 */
package com.ame.memory

class AmeEngine private constructor(private val handle: Long, private val dir: String) :
    AutoCloseable {

    companion object {
        init {
            System.loadLibrary("ame_jni")
        }

        @JvmStatic
        fun create(dir: String): AmeEngine = AmeEngine(nativeCreateSession(dir), dir)

        @JvmStatic
        private external fun nativeCreateSession(dir: String): Long

        @JvmStatic
        private external fun nativeDestroySession(handle: Long, dir: String)

        @JvmStatic
        private external fun nativeRememberTurn(
            handle: Long, speaker: String, text: String, ts: Long, diaId: String,
            sessionId: String
        ): Int

        @JvmStatic
        private external fun nativeRecall(handle: Long, query: String, limit: Int): String

        @JvmStatic
        private external fun nativeGetProfile(handle: Long): String

        @JvmStatic
        private external fun nativeStats(handle: Long): String

        @JvmStatic
        private external fun nativeSave(handle: Long, dir: String): Int

        @JvmStatic
        private external fun nativeLoad(handle: Long, dir: String): Int
    }

    val isValid: Boolean get() = handle != 0L

    /** 写入一轮对话（speaker=user/assistant；ts 为 epoch 秒） */
    fun rememberTurn(speaker: String, text: String, ts: Long, diaId: String, sessionId: String): Int =
        nativeRememberTurn(handle, speaker, text, ts, diaId, sessionId)

    /** 检索记忆证据，返回 JSON 数组 [{text,ts,score}] */
    fun recall(query: String, limit: Int): String = nativeRecall(handle, query, limit)

    /** 用户档案槽位 JSON {name,slots:[{slot,value,valid_from}]} */
    fun getProfile(): String = nativeGetProfile(handle)

    /** 引擎统计 JSON {turns,total_chars,events,nodes}（状态栏数据通路） */
    fun stats(): String = nativeStats(handle)

    fun save(): Int = nativeSave(handle, dir)

    override fun close() = nativeDestroySession(handle, dir)
}
