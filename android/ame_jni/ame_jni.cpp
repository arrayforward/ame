/*
 * 所属模块：android/ame_jni——AME 联想记忆引擎的 JNI 桥（Android App 侧唯一入口，1.x graph 后端）。
 * 本文件主体思路：
 *   引擎实例按 session 管理：nativeCreateSession(dir) 建 Engine 并立即尝试加载
 *     该 session 的快照（dir/graph.json），返回指针句柄（jlong）；每个 session 一个
 *     独立 Engine（独立记忆），destroy 时先自动 save 快照再销毁；
 *   检索/写入接口：rememberTurn 透传 Engine::remember_turn；recall 走 SeedQuery
 *     词法种子 + 默认管道，结果序列化为 JSON 数组字符串（text/ts/score）；
 *     getProfile 返回 INVOLVES 度数最高的 person 实体（用户代理）的档案槽位 JSON；
 *   快照持久化：save/load 显式导出/导入 per-session graph.json。
 * 关键算法/数据结构：句柄表 std::map<jlong, unique_ptr<Engine>> + 全局锁
 *   （ame_jni::g_mu，与 Mem 句柄表共用）；handle 单调递增值（不用裸指针值，防悬空复用）。
 *   MUTF-8 转换与句柄序号源在 jni_util.hpp（与 2.0 lmem 桥共用）。
 * 依赖关系：ame/engine/engine.h（六动词外观）、jni_util.hpp；jni.h / android log。
 */
#include <jni.h>

#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "ame/engine/engine.h"
#include "ame/extractor/extractor.h"
#include "jni_util.hpp"

namespace {

// 句柄表伪代码：long 句柄 → Engine 实例（Mem 句柄表见 mem_jni.cpp，两表共用
// ame_jni::g_mu/g_next，句柄数值不重叠）
std::map<jlong, std::unique_ptr<ame::Engine>> g_engines;

ame::Engine* get(jlong h) {
  std::lock_guard<std::mutex> lk(ame_jni::g_mu());
  auto it = g_engines.find(h);
  return it == g_engines.end() ? nullptr : it->second.get();
}

std::string snapshot_path(const std::string& dir) { return dir + "/graph.json"; }

}  // namespace

extern "C" {

// nativeCreateSession 伪代码：建 Engine（空 data_dir，不启用反馈/学习）→ 若快照存在
// 则 load 恢复记忆 → 登记句柄表返回句柄；失败返回 0。
JNIEXPORT jlong JNICALL Java_com_ame_memory_AmeEngine_nativeCreateSession(JNIEnv* env,
                                                                          jclass,
                                                                          jstring dir) {
  std::string d = ame_jni::j2s(env, dir);
  auto e = std::make_unique<ame::Engine>("");
  if (!d.empty()) {
    std::string sp = snapshot_path(d);
    FILE* f = std::fopen(sp.c_str(), "rb");
    if (f) {
      std::fclose(f);
      if (e->load(sp) != ame::Err::Ok)
        LOGE("snapshot load failed: %s", sp.c_str());
      else
        LOGI("createSession: 加载快照 %s（turns=%ld nodes=%ld）", sp.c_str(),
             e->stats().turns, e->stats().nodes);
    } else {
      LOGI("createSession: 无快照，全新记忆 %s", d.c_str());
    }
  }
  std::lock_guard<std::mutex> lk(ame_jni::g_mu());
  jlong h = ame_jni::g_next()++;
  g_engines[h] = std::move(e);
  return h;
}

// nativeDestroySession 伪代码：先 save 快照兜底 → 从句柄表移除并销毁。
JNIEXPORT void JNICALL Java_com_ame_memory_AmeEngine_nativeDestroySession(JNIEnv* env,
                                                                          jclass, jlong h,
                                                                          jstring dir) {
  std::unique_ptr<ame::Engine> e;
  {
    std::lock_guard<std::mutex> lk(ame_jni::g_mu());
    auto it = g_engines.find(h);
    if (it == g_engines.end()) return;
    e = std::move(it->second);
    g_engines.erase(it);
  }
  std::string d = ame_jni::j2s(env, dir);
  if (!d.empty()) e->save(snapshot_path(d));
  LOGI("destroySession: 快照兜底落盘 %s（turns=%ld）", d.c_str(), e->stats().turns);
}

// nativeRememberTurn 伪代码：透传 Engine::remember_turn（speaker/text/ts/diaId/sessionId），
// 返回 Err 码（0=Ok）。
JNIEXPORT jint JNICALL Java_com_ame_memory_AmeEngine_nativeRememberTurn(
    JNIEnv* env, jclass, jlong h, jstring speaker, jstring text, jlong ts, jstring dia_id,
    jstring session_id) {
  auto* e = get(h);
  if (!e) return -1;
  std::string t = ame_jni::j2s(env, text);
  auto r = e->remember_turn(ame_jni::j2s(env, speaker), t, (ame::TimePoint)ts,
                            ame_jni::j2s(env, dia_id), ame_jni::j2s(env, session_id));
  if (r.code != ame::Err::Ok)
    LOGE("rememberTurn(%s) 失败 code=%d: %.120s", ame_jni::j2s(env, speaker).c_str(),
         (int)r.code, t.c_str());
  return (jint)r.code;
}

// nativeLoadKeywordDict：运行时加载中文关键词词典（AC 自动机，带二进制缓存）。
// dict=词典文件路径（word[\tfreq][\tcat] 格式），cache=AC 缓存路径；
// 返回加载条数，<0 失败（调用方回退二元组兜底召回）。词典文件与缓存全局共享，
// 内存中的 AC 结构随各 Engine 实例从共享缓存秒开加载。
JNIEXPORT jint JNICALL Java_com_ame_memory_AmeEngine_nativeLoadKeywordDict(
    JNIEnv* env, jclass, jlong h, jstring dict, jstring cache) {
  auto* e = get(h);
  if (!e) return -1;
  int n = e->extractor().load_keyword_dict_cached(ame_jni::j2s(env, dict), "", 10000000,
                                                  ame_jni::j2s(env, cache));
  LOGI("loadKeywordDict: %d 条", n);
  return (jint)n;
}

// nativeRecall 伪代码：SeedQuery{text=query} 走默认 recall 管道（limit 截断），结果
// 序列化为 JSON 数组 [{text,ts,score}]；无引擎/无结果返回 "[]"。
// 软种子阈值降到 0.2（引擎默认 0.5）：哈希向量是词面弱基线，端上自然短查询
// 与事件的余弦普遍 <0.5，0.5 会把软通道整个废掉（bench 英文场景亦用 0.15）。
JNIEXPORT jstring JNICALL Java_com_ame_memory_AmeEngine_nativeRecall(JNIEnv* env, jclass,
                                                                     jlong h, jstring query,
                                                                     jint limit) {
  auto* e = get(h);
  if (!e) return env->NewStringUTF("[]");
  ame::SeedQuery sq;
  sq.text = ame_jni::j2s(env, query);
  sq.lex_topk = 30;
  sq.soft_min_sim = 0.2;
  auto rr = e->recall(sq, {}, 2, limit > 0 ? limit : 5, false);
  LOGD("nativeRecall: '%.60s' -> %zu 条", sq.text.c_str(), rr.events.size());
  std::string out = "[";
  bool first = true;
  for (auto& se : rr.events) {
    if (!se.event) continue;
    if (!first) out += ",";
    first = false;
    out += "{\"text\":\"" + ame_jni::jesc(se.event->text) + "\",\"ts\":" +
           std::to_string((long long)se.event->ts) +
           ",\"score\":" + std::to_string(se.score);
    auto sp = se.event->attrs.find("speaker");
    if (sp != se.event->attrs.end())
      out += ",\"speaker\":\"" + ame_jni::jesc(sp->second) + "\"";
    out += "}";
  }
  out += "]";
  return ame_jni::jstr(env, out);
}

// nativeGetProfile 伪代码：取 INVOLVES 度数最高的 person 实体（用户代理）的档案
// → JSON {name,slots:[{slot,value,valid_from}]}；无实体返回 "{}"。
JNIEXPORT jstring JNICALL Java_com_ame_memory_AmeEngine_nativeGetProfile(JNIEnv* env, jclass,
                                                                         jlong h) {
  auto* e = get(h);
  if (!e) return env->NewStringUTF("{}");
  const ame::Node* best = nullptr;
  size_t best_deg = 0;
  for (auto* n : e->storage().nodes_of_kind(ame::NodeKind::Entity)) {
    if (n->type != "person") continue;
    size_t deg = e->storage().neighbors(n->uid, {ame::RelType::INVOLVES}).size();
    if (deg > best_deg) {
      best = n;
      best_deg = deg;
    }
  }
  if (!best) return env->NewStringUTF("{}");
  auto prof = e->get_entity_profile(best->uid);
  std::string out = "{\"name\":\"" + ame_jni::jesc(best->name) + "\",\"slots\":[";
  bool first = true;
  for (auto& s : prof.slots) {
    if (s.valid_to != 0) continue;  // 只输出当前有效槽位
    if (!first) out += ",";
    first = false;
    out += "{\"slot\":\"" + ame_jni::jesc(s.slot) + "\",\"value\":\"" +
           ame_jni::jesc(s.value) +
           "\",\"valid_from\":" + std::to_string((long long)s.valid_from) + "}";
  }
  out += "]}";
  return ame_jni::jstr(env, out);
}

// nativeStats 伪代码：Engine::stats() 序列化为 JSON {turns,total_chars,events,nodes}
//（Android 状态栏数据通路：累计记忆轮数/字符数 + 引擎图规模）。
JNIEXPORT jstring JNICALL Java_com_ame_memory_AmeEngine_nativeStats(JNIEnv* env, jclass,
                                                                    jlong h) {
  auto* e = get(h);
  if (!e) return env->NewStringUTF("{}");
  auto s = e->stats();
  std::string out = "{\"turns\":" + std::to_string(s.turns) +
                    ",\"total_chars\":" + std::to_string(s.total_chars) +
                    ",\"events\":" + std::to_string(s.events) +
                    ",\"nodes\":" + std::to_string(s.nodes) + "}";
  return ame_jni::jstr(env, out);
}

// nativeSave/nativeLoad 伪代码：显式快照落盘/恢复到 dir/graph.json，返回 Err 码。
JNIEXPORT jint JNICALL Java_com_ame_memory_AmeEngine_nativeSave(JNIEnv* env, jclass, jlong h,
                                                                jstring dir) {
  auto* e = get(h);
  if (!e) return -1;
  return (jint)e->save(snapshot_path(ame_jni::j2s(env, dir)));
}

JNIEXPORT jint JNICALL Java_com_ame_memory_AmeEngine_nativeLoad(JNIEnv* env, jclass, jlong h,
                                                                jstring dir) {
  auto* e = get(h);
  if (!e) return -1;
  return (jint)e->load(snapshot_path(ame_jni::j2s(env, dir)));
}

}  // extern "C"
