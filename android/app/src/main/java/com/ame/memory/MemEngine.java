/*
 * 所属模块：app——记忆引擎公共接口（双后端抽象）。
 * 主体思路：1.x graph 引擎（AmeEngine）与 2.0 分层记忆（LmemEngine）对外同形，
 *   MemoryOrchestrator 只依赖本接口的 6 个方法；BuildConfig.MEM_STORE 决定
 *   Activity 侧实例化哪个后端。stats/getProfile 返回 JSON 文本，两后端同构
 *   （stats 均为 {turns,total_chars,events,nodes}，状态栏/诊断逻辑零分叉）。
 * 依赖关系：无（纯接口）；实现为 AmeEngine / LmemEngine。
 */
package com.ame.memory;

public interface MemEngine extends AutoCloseable {

    /** 写入一轮对话（speaker=user/assistant；ts 为 epoch 秒） */
    int rememberTurn(String speaker, String text, long ts, String diaId, String sessionId);

    /** 检索记忆证据，返回 JSON 数组 [{text,ts,score[,dia_id]}] */
    String recall(String query, int limit);

    /** 用户档案槽位 JSON */
    String getProfile();

    /** 引擎统计 JSON {turns,total_chars,events,nodes}（状态栏数据通路） */
    String stats();

    /** 快照落盘（lmem 后端为 no-op，append-only 逐条落盘） */
    int save();

    @Override
    void close();
}
