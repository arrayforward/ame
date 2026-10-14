/*
 * 所属模块：android/ame_jni——JNI 桥公共内部头（ame_jni.cpp 与 mem_jni.cpp 共用）。
 * 本文件主体思路：
 *   - 日志宏（LOG_TAG "AmeMem"）；
 *   - MUTF-8（JNI NewStringUTF/GetStringUTFChars 编码）与标准 UTF-8 互转，
 *     及 jstr/j2s/jesc 便捷封装（emoji 双向防乱码，逻辑同源 1.x 桥）；
 *   - 全局句柄序号源 g_mu/g_next：Engine 句柄表与 Mem 句柄表共用同一把锁和
 *     同一单调序号（两类句柄数值不重叠，杜绝 A 类句柄命中 B 类表的复用）。
 * 关键算法/数据结构：函数级 static + inline（C++17）保证跨翻译单元单实例。
 * 依赖关系：jni.h / android log；被 ame_jni.cpp、mem_jni.cpp 包含。
 */
#pragma once
#include <jni.h>

#include <android/log.h>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#define LOG_TAG "AmeMem"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace ame_jni {

// 句柄表全局锁与单调序号（Engine 表 / Mem 表共用同一对）
inline std::mutex& g_mu() {
  static std::mutex m;
  return m;
}
inline jlong& g_next() {
  static jlong n = 1;
  return n;
}

// MUTF-8（JNI NewStringUTF/GetStringUTFChars 的编码）与标准 UTF-8 互转。
// 唯一差异是 U+10000 以上字符（emoji 等）：标准 UTF-8 用 4 字节，MUTF-8 用
// UTF-16 代理对拆成两个 3 字节序列。直接互通会把 emoji 变成乱码——
// 出引擎文本进 NewStringUTF 前转 MUTF-8；GetStringUTFChars 拿到的转回 UTF-8。
// （NUL 的 MUTF-8 编码 0xC0 0x80 也一并处理。）
inline std::string utf8_to_mutf8(const std::string& s) {
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

inline std::string mutf8_to_utf8(const std::string& s) {
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
inline jstring jstr(JNIEnv* env, const std::string& utf8) {
  std::string m = utf8_to_mutf8(utf8);
  return env->NewStringUTF(m.c_str());
}

inline std::string j2s(JNIEnv* env, jstring js) {
  if (!js) return "";
  const char* p = env->GetStringUTFChars(js, nullptr);
  std::string s(p ? p : "");
  if (p) env->ReleaseStringUTFChars(js, p);
  return mutf8_to_utf8(s);  // MUTF-8 → UTF-8（用户输入含 emoji 时防写入乱码）
}

// JSON 字符串转义（引号/反斜杠/控制字符/换行）
inline std::string jesc(const std::string& s) {
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

}  // namespace ame_jni
