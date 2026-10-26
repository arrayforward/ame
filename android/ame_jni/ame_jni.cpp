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
 *   MUTF-8 转换与句柄序号源在 jni_util.hpp。
 * 依赖关系：ame/engine/engine.h（六动词外观）、jni_util.hpp；jni.h / android log。
 */
#include <jni.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ame/core/json.h"
#include "ame/engine/engine.h"
#include "ame/extractor/extractor.h"
#include "jni_util.hpp"

namespace {

// 句柄表伪代码：long 句柄 → Engine 实例（3.0 刀1：mem_jni 已裁，仅此一表
// ame_jni::g_mu/g_next，句柄数值不重叠）
std::map<jlong, std::unique_ptr<ame::Engine>> g_engines;

// cheap 特征逻辑重排（端侧默认档，bench --rerank cheap 的 JNI 移植）：
// 模型 JSON 由 Java 侧从 assets 注入（nativeLoadCheapModel），未注入时 recall
// 完全走引擎原序（bm25 行为）。特征与打分公式同 bench/src/ingest_answer.cpp
// cheap_score_pool：11 特征题内 minmax/排名归一 + 逻辑回归，零 LLM、纯 CPU。
struct CheapModel {
  std::vector<std::string> features;
  std::vector<double> weights;
  double bias = 0;
  bool ok = false;
};
std::map<jlong, CheapModel> g_cheap;  // 句柄 → cheap 模型（g_mu 保护）

// 内容词切词：ASCII 小写 len≥3（bench content_words 同口径）
std::vector<std::string> content_words(const std::string& text) {
  std::vector<std::string> words;
  std::string cur;
  auto flush = [&] {
    if (cur.size() >= 3) words.push_back(cur);
    cur.clear();
  };
  for (unsigned char c : text) {
    if (c < 128 && std::isalnum(c)) cur += (char)std::tolower(c);
    else flush();
  }
  flush();
  return words;
}

// 内容词 BM25-lite：df/idf 候选集内统计 log(1+N/(1+df))，Σ idf·tf/(tf+1) 按 idf_sum 归一
std::vector<double> content_scores(const std::string& query,
                                   const std::vector<ame::ScoredEvent>& evs) {
  std::vector<std::string> words = content_words(query);
  std::vector<double> scores(evs.size(), 0.0);
  if (words.empty() || evs.empty()) return scores;
  std::unordered_map<std::string, int> df;
  std::vector<std::unordered_map<std::string, int>> tfs(evs.size());
  for (size_t i = 0; i < evs.size(); ++i) {
    if (evs[i].event) for (auto& w : content_words(evs[i].event->text)) ++tfs[i][w];
    for (auto& w : words)
      if (tfs[i].count(w)) ++df[w];
  }
  double idf_sum = 0;
  std::unordered_map<std::string, double> idf;
  for (auto& w : words) {
    idf[w] = std::log(1.0 + (double)evs.size() / (1.0 + df[w]));
    idf_sum += idf[w];
  }
  for (size_t i = 0; i < evs.size(); ++i) {
    double s = 0;
    for (auto& w : words) {
      auto it = tfs[i].find(w);
      if (it != tfs[i].end()) s += idf[w] * (double)it->second / (it->second + 1.0);
    }
    scores[i] = idf_sum > 0 ? s / idf_sum : 0;
  }
  return scores;
}

// 问题专名 token：大写非句首 len≥2 小写化（bench question_entity_tokens 同口径；
// 中文查询无 ASCII 大写词 → ent_overlap 恒 0，特征自然失效）
std::vector<std::string> question_entity_tokens(const std::string& question) {
  std::vector<std::string> out;
  std::string cur;
  bool sent_start = true;
  auto flush = [&] {
    if (cur.size() >= 2 && !sent_start && std::isupper((unsigned char)cur[0])) {
      std::string low;
      for (char c : cur) low += (char)std::tolower((unsigned char)c);
      out.push_back(low);
    }
    cur.clear();
  };
  for (size_t i = 0; i <= question.size(); ++i) {
    unsigned char c = i < question.size() ? (unsigned char)question[i] : 0;
    if (c < 128 && std::isalnum(c)) {
      cur += (char)c;
    } else {
      flush();
      sent_start = (c == '.' || c == '!' || c == '?' || c == 0);
    }
    if (c == ' ') sent_start = false;
  }
  return out;
}

// cheap_rerank：全池逐候选算 11 特征逻辑分，稳定降序重排（is_entity 端侧无实体
// 通道候选集恒 0——训练数据同恒 0、权重 0，安全外推）
void cheap_rerank(const CheapModel& m, const std::string& query,
                  std::vector<ame::ScoredEvent>& evs) {
  size_t n = evs.size();
  if (n < 2) return;
  std::vector<double> cs = content_scores(query, evs);
  auto minmax = [](double v, double lo, double hi) { return hi > lo ? (v - lo) / (hi - lo) : 0.0; };
  std::vector<double> fus(n), gra(n), ctx(n), tim(n), rec(n);
  double bm_max = 0;
  for (size_t i = 0; i < n; ++i) {
    fus[i] = evs[i].score;
    gra[i] = evs[i].graph_score;
    ctx[i] = evs[i].context_score;
    tim[i] = evs[i].time_score;
    rec[i] = evs[i].event ? -(double)evs[i].event->ts : 0;
    bm_max = std::max(bm_max, cs[i]);
  }
  auto span = [&](const std::vector<double>& v, double& lo, double& hi) {
    lo = hi = v[0];
    for (double x : v) { lo = std::min(lo, x); hi = std::max(hi, x); }
  };
  double f_lo, f_hi, g_lo, g_hi, c_lo, c_hi, t_lo, t_hi, r_lo, r_hi;
  span(fus, f_lo, f_hi); span(gra, g_lo, g_hi); span(ctx, c_lo, c_hi);
  span(tim, t_lo, t_hi); span(rec, r_lo, r_hi);
  std::vector<size_t> ord_bm(n), ord_fu(n);
  for (size_t i = 0; i < n; ++i) ord_bm[i] = ord_fu[i] = i;
  std::stable_sort(ord_bm.begin(), ord_bm.end(), [&](size_t a, size_t b) { return cs[a] > cs[b]; });
  std::stable_sort(ord_fu.begin(), ord_fu.end(), [&](size_t a, size_t b) { return fus[a] > fus[b]; });
  std::vector<double> rank_bm(n), rank_fu(n);
  for (size_t r = 0; r < n; ++r) {
    rank_bm[ord_bm[r]] = 1.0 - (double)r / (double)(n - 1);
    rank_fu[ord_fu[r]] = 1.0 - (double)r / (double)(n - 1);
  }
  if (bm_max <= 0) bm_max = 1.0;
  const std::vector<std::string> q_ents = question_entity_tokens(query);
  std::vector<std::pair<double, size_t>> scored(n);
  for (size_t i = 0; i < n; ++i) {
    std::unordered_set<std::string> ew;
    if (evs[i].event) for (auto& w : content_words(evs[i].event->text)) ew.insert(w);
    double ent_ov = 0.0;
    if (!q_ents.empty()) {
      int hit = 0;
      for (auto& t : q_ents)
        if (ew.count(t)) ++hit;
      ent_ov = (double)hit / (double)q_ents.size();
    }
    std::unordered_map<std::string, double> x = {
        {"bm25", cs[i]},
        {"bm25_rank", rank_bm[i]},
        {"fusion", minmax(fus[i], f_lo, f_hi)},
        {"graph", minmax(gra[i], g_lo, g_hi)},
        {"ctx", minmax(ctx[i], c_lo, c_hi)},
        {"time", minmax(tim[i], t_lo, t_hi)},
        {"fusion_rank", rank_fu[i]},
        {"is_entity", 0.0},
        {"recency", minmax(rec[i], r_lo, r_hi)},
        {"bm25_share", cs[i] / bm_max},
        {"ent_overlap", ent_ov},
    };
    double z = m.bias;
    for (size_t k = 0; k < m.features.size(); ++k) z += m.weights[k] * x[m.features[k]];
    double s = 1.0 / (1.0 + std::exp(-std::max(-30.0, std::min(30.0, z))));
    scored[i] = {s, i};
  }
  std::stable_sort(scored.begin(), scored.end(),
                   [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<ame::ScoredEvent> out;
  out.reserve(n);
  for (auto& p : scored) out.push_back(evs[p.second]);
  evs = std::move(out);
}

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
    g_cheap.erase(h);
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

// nativeLoadCheapModel：注入 cheap 重排模型 JSON（features/weights/bias），
// 之后 nativeRecall 自动走 池60→cheap→topN；解析失败记 ok=false 并回退引擎原序。
JNIEXPORT jint JNICALL Java_com_ame_memory_AmeEngine_nativeLoadCheapModel(
    JNIEnv* env, jclass, jlong h, jstring model_json) {
  auto* e = get(h);
  if (!e) return -1;
  bool ok = false;
  ame::Json j = ame::Json::parse(ame_jni::j2s(env, model_json), &ok);
  CheapModel m;
  if (ok) {
    const auto* feats = j.find("features");
    const auto* ws = j.find("weights");
    const auto* b = j.find("bias");
    if (feats && ws && feats->kind == ame::Json::Arr && ws->kind == ame::Json::Arr) {
      for (auto& f : feats->arr) m.features.push_back(f.as_str());
      for (auto& w : ws->arr) m.weights.push_back(w.as_num());
      m.bias = b ? b->as_num() : 0;
      m.ok = !m.features.empty() && m.features.size() == m.weights.size();
    }
  }
  LOGI("loadCheapModel: %s（%zu 特征）", m.ok ? "已启用" : "解析失败，回退引擎原序",
       m.features.size());
  int rc = m.ok ? 1 : 0;
  std::lock_guard<std::mutex> lk(ame_jni::g_mu());
  g_cheap[h] = std::move(m);
  return rc;
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
  CheapModel cm;
  {
    std::lock_guard<std::mutex> lk(ame_jni::g_mu());
    auto it = g_cheap.find(h);
    if (it != g_cheap.end()) cm = it->second;  // 拷贝出锁，recall 不持锁
  }
  bool cheap = cm.ok && limit > 0;
  // cheap 档：池放大到 60（与 bench llm/cheap 档候选量一致）→ 特征重排 → 截 limit
  int pool = cheap ? std::max((int)limit, 60) : (limit > 0 ? limit : 5);
  auto rr = e->recall(sq, {}, 2, pool, false);
  if (cheap) {
    cheap_rerank(cm, sq.text, rr.events);
    if (rr.events.size() > (size_t)limit) rr.events.resize((size_t)limit);
  }
  LOGD("nativeRecall: '%.60s' -> %zu 条（cheap=%d 池=%d）", sq.text.c_str(), rr.events.size(),
       cheap ? 1 : 0, pool);
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
