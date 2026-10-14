/*
 * 所属模块：android/ame_jni——AME 联想记忆引擎的 JNI 桥（Android App 侧唯一入口）。
 * 本文件主体思路：
 *   引擎实例按 session 管理：nativeCreateSession(dir) 建 Engine 并立即尝试加载
 *     该 session 的快照（dir/graph.json），返回指针句柄（jlong）；每个 session 一个
 *     独立 Engine（独立记忆），destroy 时先自动 save 快照再销毁；
 *   检索/写入接口：rememberTurn 透传 Engine::remember_turn；recall 走 SeedQuery
 *     词法种子 + 默认管道，结果序列化为 JSON 数组字符串（text/ts/score）；
 *     getProfile 返回 INVOLVES 度数最高的 person 实体（用户代理）的档案槽位 JSON；
 *   快照持久化：save/load 显式导出/导入 per-session graph.json。
 * 关键算法/数据结构：句柄表 std::map<jlong, unique_ptr<Engine>> + mutex（JNI 多线程
 *   回调安全）；JSON 序列化用核心层极简写法（手写转义，避免引入依赖）。
 * 依赖关系：ame/engine/engine.h（六动词外观）；jni.h / android log。
 */
#include <jni.h>

#include <android/log.h>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "ame/engine/engine.h"
#include "ame/extractor/extractor.h"

#define LOG_TAG "AmeMem"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// 句柄表伪代码：long 句柄 → Engine 实例；handle 用单调递增值（不用裸指针值，防悬空复用）
std::mutex g_mu;
std::map<jlong, std::unique_ptr<ame::Engine>> g_engines;
jlong g_next = 1;

ame::Engine* get(jlong h) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_engines.find(h);
  return it == g_engines.end() ? nullptr : it->second.get();
}

// MUTF-8（JNI NewStringUTF/GetStringUTFChars 的编码）与标准 UTF-8 互转。
// 唯一差异是 U+10000 以上字符（emoji 等）：标准 UTF-8 用 4 字节，MUTF-8 用
// UTF-16 代理对拆成两个 3 字节序列。直接互通会把 emoji 变成乱码——
// 出引擎文本进 NewStringUTF 前转 MUTF-8；GetStringUTFChars 拿到的转回 UTF-8。
// （NUL 的 MUTF-8 编码 0xC0 0x80 也一并处理。）
std::string utf8_to_mutf8(const std::string& s) {
  std::string o;
  auto push_utf8 = [&](unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) {
      o += (char)(0xC0 | (cp >> 6));
      o += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      o += (char)(0xE0 | (cp >> 12));
      o += (char)(0x80 | ((cp >> 6) & 0x3F));
      o += (char)(0x80 | (cp & 0x3F));
    } else {
      o += (char)(0xF0 | (cp >> 18));
      o += (char)(0x80 | ((cp >> 12) & 0x3F));
      o += (char)(0x80 | ((cp >> 6) & 0x3F));
      o += (char)(0x80 | (cp & 0x3F));
    }
  };
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    unsigned cp;
    size_t len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { o += s[i++]; continue; }  // 非法字节原样透传
    bool ok = i + len <= s.size();
    for (size_t j = 1; ok && j < len; ++j) {
      unsigned char cc = (unsigned char)s[i + j];
      if ((cc & 0xC0) != 0x80) ok = false;
      else cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) { o += s[i++]; continue; }
    i += len;
    if (cp == 0) { o += "\xC0\x80"; continue; }        // NUL 的 MUTF-8 编码
    if (cp >= 0x10000) {                                // 代理对拆分
      cp -= 0x10000;
      push_utf8(0xD800 + (cp >> 10));
      push_utf8(0xDC00 + (cp & 0x3FF));
    } else {
      push_utf8(cp);
    }
  }
  return o;
}

std::string mutf8_to_utf8(const std::string& s) {
  std::string o;
  auto push_utf8 = [&](unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) {
      o += (char)(0xC0 | (cp >> 6));
      o += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      o += (char)(0xE0 | (cp >> 12));
      o += (char)(0x80 | ((cp >> 6) & 0x3F));
      o += (char)(0x80 | (cp & 0x3F));
    } else {
      o += (char)(0xF0 | (cp >> 18));
      o += (char)(0x80 | ((cp >> 12) & 0x3F));
      o += (char)(0x80 | ((cp >> 6) & 0x3F));
      o += (char)(0x80 | (cp & 0x3F));
    }
  };
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    unsigned cp;
    size_t len;
    if (c == 0xC0 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0x80) {
      o += '\0'; i += 2; continue;                      // MUTF-8 的 NUL
    }
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { o += s[i++]; continue; }
    bool ok = i + len <= s.size();
    for (size_t j = 1; ok && j < len; ++j) {
      unsigned char cc = (unsigned char)s[i + j];
      if ((cc & 0xC0) != 0x80) ok = false;
      else cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) { o += s[i++]; continue; }
    i += len;
    // 代理对合并回 4 字节 UTF-8（MUTF-8 的 4 字节字符是代理对形态）
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 2 < s.size() + 1) {
      unsigned char c2 = i < s.size() ? (unsigned char)s[i] : 0;
      if ((c2 & 0xF0) == 0xE0) {
        unsigned lo = c2 & 0x0F;
        bool ok2 = true;
        for (size_t j = 1; ok2 && j < 3; ++j) {
          unsigned char cc = (unsigned char)s[i + j];
          if ((cc & 0xC0) != 0x80) ok2 = false;
          else lo = (lo << 6) | (cc & 0x3F);
        }
        if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
          i += 3;
          push_utf8(0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00));
          continue;
        }
      }
    }
    push_utf8(cp);
  }
  return o;
}

// jstr：引擎侧 UTF-8 文本 → Java 字符串（经 MUTF-8 转换，防 emoji 乱码）
jstring jstr(JNIEnv* env, const std::string& utf8) {
  std::string m = utf8_to_mutf8(utf8);
  return env->NewStringUTF(m.c_str());
}

std::string j2s(JNIEnv* env, jstring js) {
  if (!js) return "";
  const char* p = env->GetStringUTFChars(js, nullptr);
  std::string s(p ? p : "");
  if (p) env->ReleaseStringUTFChars(js, p);
  return mutf8_to_utf8(s);  // MUTF-8 → UTF-8（用户输入含 emoji 时防写入乱码）
}

// JSON 字符串转义（引号/反斜杠/控制字符/换行）
std::string jesc(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else {
          o += c;
        }
    }
  }
  return o;
}

std::string snapshot_path(const std::string& dir) { return dir + "/graph.json"; }

}  // namespace

extern "C" {

// nativeCreateSession 伪代码：建 Engine（空 data_dir，不启用反馈/学习）→ 若快照存在
// 则 load 恢复记忆 → 登记句柄表返回句柄；失败返回 0。
JNIEXPORT jlong JNICALL Java_com_ame_memory_AmeEngine_nativeCreateSession(JNIEnv* env,
                                                                          jclass,
                                                                          jstring dir) {
  std::string d = j2s(env, dir);
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
  std::lock_guard<std::mutex> lk(g_mu);
  jlong h = g_next++;
  g_engines[h] = std::move(e);
  return h;
}

// nativeDestroySession 伪代码：先 save 快照兜底 → 从句柄表移除并销毁。
JNIEXPORT void JNICALL Java_com_ame_memory_AmeEngine_nativeDestroySession(JNIEnv* env,
                                                                          jclass, jlong h,
                                                                          jstring dir) {
  std::unique_ptr<ame::Engine> e;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_engines.find(h);
    if (it == g_engines.end()) return;
    e = std::move(it->second);
    g_engines.erase(it);
  }
  std::string d = j2s(env, dir);
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
  std::string t = j2s(env, text);
  auto r = e->remember_turn(j2s(env, speaker), t, (ame::TimePoint)ts,
                            j2s(env, dia_id), j2s(env, session_id));
  if (r.code != ame::Err::Ok)
    LOGE("rememberTurn(%s) 失败 code=%d: %.120s", j2s(env, speaker).c_str(),
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
  int n = e->extractor().load_keyword_dict_cached(j2s(env, dict), "", 10000000,
                                                  j2s(env, cache));
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
  sq.text = j2s(env, query);
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
    out += "{\"text\":\"" + jesc(se.event->text) + "\",\"ts\":" +
           std::to_string((long long)se.event->ts) +
           ",\"score\":" + std::to_string(se.score);
    auto sp = se.event->attrs.find("speaker");
    if (sp != se.event->attrs.end()) out += ",\"speaker\":\"" + jesc(sp->second) + "\"";
    out += "}";
  }
  out += "]";
  return jstr(env, out);
}

// nativeGetProfile 伪代码：取 INVOLVES 度数最高的 person 实体（用户代理）的档案
// → JSON {name, slots:[{slot,value,valid_from}]}；无实体返回 "{}"。
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
  std::string out = "{\"name\":\"" + jesc(best->name) + "\",\"slots\":[";
  bool first = true;
  for (auto& s : prof.slots) {
    if (s.valid_to != 0) continue;  // 只输出当前有效槽位
    if (!first) out += ",";
    first = false;
    out += "{\"slot\":\"" + jesc(s.slot) + "\",\"value\":\"" + jesc(s.value) +
           "\",\"valid_from\":" + std::to_string((long long)s.valid_from) + "}";
  }
  out += "]}";
  return jstr(env, out);
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
  return jstr(env, out);
}

// nativeSave/nativeLoad 伪代码：显式快照落盘/恢复到 dir/graph.json，返回 Err 码。
JNIEXPORT jint JNICALL Java_com_ame_memory_AmeEngine_nativeSave(JNIEnv* env, jclass, jlong h,
                                                                jstring dir) {
  auto* e = get(h);
  if (!e) return -1;
  return (jint)e->save(snapshot_path(j2s(env, dir)));
}

JNIEXPORT jint JNICALL Java_com_ame_memory_AmeEngine_nativeLoad(JNIEnv* env, jclass, jlong h,
                                                                jstring dir) {
  auto* e = get(h);
  if (!e) return -1;
  return (jint)e->load(snapshot_path(j2s(env, dir)));
}

}  // extern "C"
