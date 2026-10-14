/*
 * 所属模块：android/ame_jni——AME 2.0 分层记忆（M15–M19）的 JNI 桥（lmem 后端）。
 * 本文件主体思路：
 *   Mem 实例按 session 管理：nativeCreate(rootDir=sessionDir/lmem, owner=sessionId)
 *     建 Mem 网关（观测 append-only 逐条落盘，打开时 replay 恢复索引）；
 *   自动迁移：session 目录存在 1.x graph.json 且 lmem 分片为空且未写 migrated.flag
 *     → mem::migrate_graph 把 Event 节点转成观测（写标记防重复，迁移日志 LOGI）；
 *   接口面与 AmeEngine 同形（remember/recall/profile/stats/erase/save/close），
 *     recall 证据按 EvidenceItem.ref_obs → Mem::obs 回溯 meta.dia_id；
 *   save 为 no-op（lmem 无全量快照概念，remember 即落盘）；close 只释放句柄。
 * 关键算法/数据结构：句柄表 std::map<jlong, shared_ptr<LmemCtx>> + 全局锁
 *   （ame_jni::g_mu，与 Engine 表共用序号源）；LmemCtx 内嵌 per-handle mutex
 *   串行化本句柄全部调用（Mem 非线程安全；App 单 io 线程外再加一层兜底，
 *   与 close 并发时先等在途调用完成再释放）。
 * 依赖关系：ame/mem/mem.h（M19 网关 + migrate_graph）；jni_util.hpp；android log。
 */
#include <jni.h>

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "ame/mem/mem.h"
#include "jni_util.hpp"

namespace {

// Mem 句柄上下文：网关实例 + owner + 本句柄串行锁
struct LmemCtx {
  std::unique_ptr<ame::Mem> mem;
  std::string owner;
  std::mutex mu;
};

// Mem 句柄表（与 Engine 表共用 ame_jni::g_mu/g_next，句柄数值不重叠）
std::map<jlong, std::shared_ptr<LmemCtx>> g_mems;

// get 伪代码：全局锁下取 shared_ptr 副本——即使并发 close 把表项移除，
// 调用方仍持有上下文，不会在调用中途被释放（close 释放前会等在途调用完成）
std::shared_ptr<LmemCtx> get_mem(jlong h) {
  std::lock_guard<std::mutex> lk(ame_jni::g_mu());
  auto it = g_mems.find(h);
  return it == g_mems.end() ? nullptr : it->second;
}

void write_flag(const std::string& root) {
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  std::ofstream f(ame::migrate_flag_path(root), std::ios::binary | std::ios::trunc);
  f << "ok\n";
}

// 回溯证据来源观测的 dia_id（Observation.meta）
std::string dia_id_of(const ame::Mem& m, uint64_t ref_obs) {
  if (!ref_obs) return "";
  const ame::Observation* o = m.obs(ref_obs);
  if (!o) return "";
  const ame::Json* d = o->meta.find("dia_id");
  return d ? d->as_str() : "";
}

}  // namespace

extern "C" {

// nativeCreate 伪代码：建 Mem（rootDir=session 目录下 lmem/ 子目录，owner=sessionId）
//   步骤1：自动迁移判定——graph.json 存在 && migrated.flag 未写（不判分片空否：
//         中断重启必须重做，migrate_graph 幂等去重保证不重复）；
//   步骤2：命中则 migrate_graph 逐 Event 转观测，写完写 migrated.flag（防重复）；
//   步骤3：登记句柄表返回句柄（失败返回 0 由 Java isValid 兜底）。
JNIEXPORT jlong JNICALL Java_com_ame_memory_LmemEngine_nativeCreate(JNIEnv* env, jclass,
                                                                    jstring root_dir,
                                                                    jstring owner) {
  std::string root = ame_jni::j2s(env, root_dir);
  std::string ow = ame_jni::j2s(env, owner);
  auto ctx = std::make_shared<LmemCtx>();
  ctx->owner = ow;
  ctx->mem = std::make_unique<ame::Mem>(root);

  // 迁移条件：graph.json 存在且 migrated.flag 未写。不判"分片是否为空"——
  // 迁移中断（部分分片已写、flag 未写）重启时必须重做；migrate_graph 幂等去重，
  // 重复执行不会写入重复观测（蓝方自检：迁移中断安全）
  std::error_code ec;
  const std::string session_dir = std::filesystem::path(root).parent_path().string();
  const std::string graph = session_dir + "/graph.json";
  if (!ow.empty() && std::filesystem::exists(graph, ec) && !ame::migrate_done(root)) {
    LOGI("memCreate: 检测到 1.x 快照 %s，开始迁移 owner=%s", graph.c_str(), ow.c_str());
    const size_t n = ame::migrate_graph(*ctx->mem, graph, ow, ow);  // channel=session_id
    LOGI("memCreate: 迁移完成 %zu 条观测（owner=%s）", n, ow.c_str());
    write_flag(root);
  }
  std::lock_guard<std::mutex> lk(ame_jni::g_mu());
  jlong h = ame_jni::g_next()++;
  g_mems[h] = ctx;
  return h;
}

// nativeRemember：透传 Mem::remember（owner 为句柄创建时传入的 sessionId），
// 返回 Err 码（0=Ok）；观测 append-only 立即落盘，无需后续 save。
JNIEXPORT jint JNICALL Java_com_ame_memory_LmemEngine_nativeRemember(JNIEnv* env, jclass,
                                                                     jlong h, jstring channel,
                                                                     jstring kind, jstring text,
                                                                     jlong ts) {
  auto c = get_mem(h);
  if (!c) return -1;
  std::lock_guard<std::mutex> lk(c->mu);
  std::string t = ame_jni::j2s(env, text);
  auto r = c->mem->remember(c->owner, ame_jni::j2s(env, channel),
                            ame_jni::j2s(env, kind), t, (int64_t)ts);
  if (r.code != ame::Err::Ok)
    LOGE("memRemember(%s) 失败 code=%d: %.120s", c->owner.c_str(), (int)r.code,
         t.c_str());
  return (jint)r.code;
}

// nativeRecall 伪代码：Mem::recall（视图层合成，零写副作用），结果序列化为
// JSON 数组 [{text,ts,score,dia_id}]——dia_id 经 EvidenceItem.ref_obs 回溯
// Observation.meta；无句柄/无结果返回 "[]"。
JNIEXPORT jstring JNICALL Java_com_ame_memory_LmemEngine_nativeRecall(JNIEnv* env, jclass,
                                                                      jlong h, jstring query,
                                                                      jint limit) {
  auto c = get_mem(h);
  if (!c) return env->NewStringUTF("[]");
  std::lock_guard<std::mutex> lk(c->mu);
  const std::string q = ame_jni::j2s(env, query);
  auto items = c->mem->recall(q, c->owner, limit > 0 ? limit : 5, (int64_t)std::time(nullptr));
  LOGD("memRecall: '%.60s' -> %zu 条", q.c_str(), items.size());
  std::string out = "[";
  bool first = true;
  for (auto& it : items) {
    if (!first) out += ",";
    first = false;
    out += "{\"text\":\"" + ame_jni::jesc(it.text) + "\",\"ts\":" +
           std::to_string((long long)it.ts) + ",\"score\":" +
           std::to_string(it.score) + ",\"dia_id\":\"" +
           ame_jni::jesc(dia_id_of(*c->mem, it.ref_obs)) + "\"}";
  }
  out += "]";
  return ame_jni::jstr(env, out);
}

// nativeProfile：Mem::profile 的 Json 直出（fold 物化最新视图）。
JNIEXPORT jstring JNICALL Java_com_ame_memory_LmemEngine_nativeProfile(JNIEnv* env, jclass,
                                                                       jlong h) {
  auto c = get_mem(h);
  if (!c) return env->NewStringUTF("{}");
  std::lock_guard<std::mutex> lk(c->mu);
  return ame_jni::jstr(env, c->mem->profile(c->owner, (int64_t)std::time(nullptr)).dump());
}

// nativeStats 伪代码：观测层统计 → JSON {turns,total_chars,events,nodes}
//（与 1.x 状态栏数据通路同构：turns=观测数，total_chars=观测文本字符合计，
//   events=观测数，nodes=事实槽位数——profile fold 的 entity→attr 叶数）。
JNIEXPORT jstring JNICALL Java_com_ame_memory_LmemEngine_nativeStats(JNIEnv* env, jclass,
                                                                     jlong h) {
  auto c = get_mem(h);
  if (!c) return env->NewStringUTF("{}");
  std::lock_guard<std::mutex> lk(c->mu);
  size_t turns = 0, total_chars = 0;
  c->mem->obs_store().for_owner(c->owner, [&](const ame::Observation& o) {
    ++turns;
    total_chars += o.text.size();
  });
  size_t nodes = 0;
  const ame::Json p = c->mem->profile(c->owner, (int64_t)std::time(nullptr));
  for (auto& ent : p.obj)
    nodes += ent.second.obj.size();
  std::string out = "{\"turns\":" + std::to_string(turns) +
                    ",\"total_chars\":" + std::to_string(total_chars) +
                    ",\"events\":" + std::to_string(turns) +
                    ",\"nodes\":" + std::to_string(nodes) + "}";
  return ame_jni::jstr(env, out);
}

// nativeEraseChannel：删除该会话的观测并级联事实（Mem::eraseChannel 两级语义），
// 返回剔除的观测数。
JNIEXPORT jint JNICALL Java_com_ame_memory_LmemEngine_nativeEraseChannel(JNIEnv* env, jclass,
                                                                         jlong h,
                                                                         jstring channel) {
  auto c = get_mem(h);
  if (!c) return -1;
  std::lock_guard<std::mutex> lk(c->mu);
  return (jint)c->mem->eraseChannel(c->owner, ame_jni::j2s(env, channel));
}

// nativeSave：lmem 为逐条 append 落盘，无全量快照概念——no-op 返回 0
//（保留接口只为与 1.x 引擎面同形，orchestrator 的快照节流调用无副作用）。
JNIEXPORT jint JNICALL Java_com_ame_memory_LmemEngine_nativeSave(JNIEnv*, jclass, jlong h) {
  auto c = get_mem(h);
  if (!c) return -1;
  return 0;
}

// nativeClose 伪代码：全局锁下摘表（后续调用拿不到句柄）→ 取本句柄锁
// 等在途调用完成 → 释放 Mem。无快照兜底（append-only 早已落盘）。
JNIEXPORT void JNICALL Java_com_ame_memory_LmemEngine_nativeClose(JNIEnv*, jclass, jlong h) {
  std::shared_ptr<LmemCtx> c;
  {
    std::lock_guard<std::mutex> lk(ame_jni::g_mu());
    auto it = g_mems.find(h);
    if (it == g_mems.end()) return;
    c = std::move(it->second);
    g_mems.erase(it);
  }
  std::lock_guard<std::mutex> lk(c->mu);
  c->mem.reset();
  LOGI("memClose: 释放 lmem 句柄 %ld（owner=%s）", (long)h, c->owner.c_str());
}

}  // extern "C"
