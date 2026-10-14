/*
 * 所属模块：llm 接入层——DeepSeekClient 实现。
 * 本文件主体思路：
 *   传输：curl 子进程（请求体与含鉴权的 curl 配置写入临时文件，避免 key 出现在
 *     进程命令行）；HTTP 状态码经 -w 取回；
 *   缓存：请求哈希 → cache_dir/<hash>.txt，命中直接返回（断点续跑零重复扣费）；
 *   重试：429/5xx/传输失败指数退避（1/2/4/8/16s，最多 5 次），其余 4xx 立即失败；
 *   并发：chat_batch 用固定线程池 + 原子工作索引。
 * 关键算法/数据结构：FNV-1a 64 位哈希做缓存键；临时文件用 mkstemp。
 * 依赖关系：依赖 core(json.h) 做响应解析；被 components 与 bench 依赖。
 * 安全约束：api_key 只写入临时 curl 配置文件（0600 权限），不打印、不拼进命令行字符串。
 */
#include "ame/llm/llm.h"

#include "ame/core/json.h"
#include "ame/core/log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace ame {

// FNV-1a 64 位（缓存键用，进程间稳定）
static uint64_t fnv64(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  return h;
}

// curl 子进程传输伪代码：
//   步骤1：请求体写入临时文件 body.json（避免 shell 转义问题）；
//   步骤2：curl 配置写入临时文件（含 Authorization 头，0600 权限，key 不进命令行）；
//   步骤3：popen 执行 curl（-sS 静默、-m 120 超时、-w 追加 HTTP 状态码），读取全部输出；
//   步骤4：拆分响应体与状态码返回；临时文件删除。
static int curl_transport(const std::string& base_url, const std::string& api_key,
                          const std::string& body, std::string* out) {
  char body_path[] = "/tmp/ame_llm_body_XXXXXX";
  char conf_path[] = "/tmp/ame_llm_conf_XXXXXX";
  int bfd = mkstemp(body_path);
  int cfd = mkstemp(conf_path);
  if (bfd < 0 || cfd < 0) return 0;
  {
    std::ofstream bf(body_path);
    bf << body;
    std::ofstream cf(conf_path);
    cf << "header = \"Authorization: Bearer " << api_key << "\"\n";
    cf << "header = \"Content-Type: application/json\"\n";
  }
  close(bfd);
  close(cfd);
  // 注意：格式串必须保持反斜杠-n 两字面字符（C 串 \\n），由 curl 自行解释——
  // 若用 C 转义 '\n'，运行时串内嵌真实换行，cmd.exe 按空白切分会毁掉 -w 参数
  // （WSL/sh 下引号保护换行所以历史正常；原生 Windows 下此前一律 http 0）。
  std::string cmd = "curl -sS -m 120 -K " + std::string(conf_path) + " -X POST " + base_url +
                    "/chat/completions --data @" + body_path + " -w \"\\n%{http_code}\"";
  std::string raw;
  FILE* fp = popen(cmd.c_str(), "r");
  if (fp) {
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), fp)) raw.append(buf, n);
  }
  int rc = fp ? pclose(fp) : -1;
  std::remove(body_path);
  std::remove(conf_path);
  if (rc != 0 || raw.empty()) return 0;
  size_t nl = raw.rfind('\n');
  if (nl == std::string::npos) return 0;
  *out = raw.substr(0, nl);
  return std::atoi(raw.c_str() + nl + 1);
}

DeepSeekClient::DeepSeekClient(std::string base_url, std::string api_key, std::string model,
                               std::string cache_dir)
    : base_url_(std::move(base_url)),
      api_key_(std::move(api_key)),
      model_(std::move(model)),
      cache_dir_(std::move(cache_dir)) {
  transport_ = [this](const std::string& body, std::string* out) {
    return curl_transport(base_url_, api_key_, body, out);
  };
}

// cache_key 伪代码：拼接 模型名 + 各消息(role:content) + 选项 → FNV-1a 十六进制。
std::string DeepSeekClient::cache_key(const std::vector<ChatMessage>& messages,
                                      const ChatOptions& opts) const {
  std::string s = model_;
  for (auto& m : messages) { s += "\n" + m.role + ":" + m.content; }
  s += "\nT" + std::to_string(opts.temperature) + "M" + std::to_string(opts.max_tokens) +
       (opts.json ? "J" : "") + (opts.disable_thinking ? "D" : "");
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)fnv64(s));
  return hex;
}

// build_body 伪代码：用 core Json 组装 OpenAI 兼容请求体（model/messages/温度/max_tokens，
//   json 模式附加 response_format，disable_thinking 时附加 thinking:{type:disabled}）。
std::string DeepSeekClient::build_body(const std::vector<ChatMessage>& messages,
                                       const ChatOptions& opts) const {
  Json root = Json::object();
  root["model"] = Json::string(model_);
  Json ms = Json::array();
  for (auto& m : messages) {
    Json jm = Json::object();
    jm["role"] = Json::string(m.role);
    jm["content"] = Json::string(m.content);
    ms.push(std::move(jm));
  }
  root["messages"] = std::move(ms);
  root["temperature"] = Json::number(opts.temperature);
  root["max_tokens"] = Json::number(opts.max_tokens);
  if (opts.json) {
    Json rf = Json::object();
    rf["type"] = Json::string("json_object");
    root["response_format"] = std::move(rf);
  }
  if (opts.disable_thinking) {
    // deepseek-v4 推理模型：关思考（实测有效，避免 reasoning 烧光 max_tokens）
    Json th = Json::object();
    th["type"] = Json::string("disabled");
    root["thinking"] = std::move(th);
  }
  return root.dump();
}

// extract_content 已由 call_api 内联解析取代（需要同时读 finish_reason/usage）。

// call_api 伪代码（重试策略）：
//   步骤1：按当前 opts 组装请求体并执行传输，取回 HTTP 状态码与响应体；
//   步骤2：200 → 解析 content 与 finish_reason；
//          若 finish_reason==length（输出被截断，含思考烧光预算情形）且 max_tokens
//          未到上限 32768，则翻倍 max_tokens 立即重试（记日志：finish_reason、
//          completion_tokens、内容前缀，不打印 key）；
//   步骤3：429/5xx/传输失败 → 指数退避（1/2/4/8/16 秒）重试，最多 5 次；
//   步骤4：其余 4xx 立即失败（请求本身有问题，重试无意义）。
std::string DeepSeekClient::call_api(const std::vector<ChatMessage>& messages,
                                     ChatOptions opts) {
  for (int attempt = 0; attempt < 5; ++attempt) {
    if (attempt > 0) {
      std::this_thread::sleep_for(std::chrono::seconds(1 << (attempt - 1)));
    }
    std::string out;
    int code = transport_(build_body(messages, opts), &out);
    ++api_calls_;
    if (code == 200) {
      bool ok = false;
      Json j = Json::parse(out, &ok);
      const Json* choices = ok ? j.find("choices") : nullptr;
      if (!choices || choices->arr.empty()) {
        AME_LOGW("llm: 200 但响应解析失败，body[:200]=%s", out.substr(0, 200).c_str());
        return "";
      }
      const Json* ch = &choices->arr[0];
      const Json* fr = ch->find("finish_reason");
      std::string finish = fr ? fr->as_str() : "";
      const Json* msg = ch->find("message");
      const Json* content = msg ? msg->find("content") : nullptr;
      std::string c = content ? content->as_str() : "";
      const Json* usage = j.find("usage");
      long comp = 0;
      if (usage) {
        const Json* ct = usage->find("completion_tokens");
        if (ct) comp = (long)ct->as_num();
      }
      if (finish == "length" && opts.max_tokens < 32768) {
        opts.max_tokens *= 2;  // 截断自动加大预算重试
        AME_LOGW("llm: finish_reason=length completion=%ld content[:200]=%s，max_tokens 翻倍重试",
                 comp, c.substr(0, 200).c_str());
        continue;
      }
      if (c.empty())
        AME_LOGW("llm: 200 但 content 为空，finish_reason=%s completion=%ld",
                 finish.c_str(), comp);
      return c;
    }
    if (code == 429 || code >= 500 || code == 0) {
      AME_LOGW("llm: http %d，第 %d 次重试", code, attempt + 1);
      continue;
    }
    AME_LOGE("llm: http %d 不可重试错误，body[:200]=%s", code, out.substr(0, 200).c_str());
    return "";
  }
  return "";
}

// chat 伪代码：
//   步骤1：算缓存键，命中缓存文件直接返回（cache_hits_+1）；
//   步骤2：未命中 → call_api；成功则写缓存文件（供断点续跑）后返回。
std::string DeepSeekClient::chat(const std::vector<ChatMessage>& messages,
                                 const ChatOptions& opts) {
  std::string key = cache_key(messages, opts);
  if (!cache_dir_.empty()) {
    std::ifstream f(cache_dir_ + "/" + key + ".txt", std::ios::binary);
    if (f) {
      ++cache_hits_;
      std::stringstream ss;
      ss << f.rdbuf();
      return ss.str();
    }
  }
  std::string resp = call_api(messages, opts);
  if (!resp.empty() && !cache_dir_.empty()) {
    std::string path = cache_dir_ + "/" + key + ".txt";
    std::ofstream f(path + ".tmp", std::ios::binary | std::ios::trunc);
    f << resp;
    f.close();
    std::rename((path + ".tmp").c_str(), path.c_str());  // 原子落盘
  }
  return resp;
}

// chat_batch 伪代码：固定线程池（concurrency 个线程）+ 原子索引领取任务，
//   结果按下标写回，保持与请求同序。
std::vector<std::string> DeepSeekClient::chat_batch(const std::vector<ChatRequest>& reqs,
                                                    int concurrency) {
  std::vector<std::string> results(reqs.size());
  std::atomic<size_t> idx{0};
  if (concurrency < 1) concurrency = 1;
  int nthreads = std::min<int>(concurrency, (int)reqs.size());
  std::vector<std::thread> pool;
  for (int t = 0; t < nthreads; ++t) {
    pool.emplace_back([&] {
      for (;;) {
        size_t i = idx.fetch_add(1);
        if (i >= reqs.size()) break;
        results[i] = chat(reqs[i].messages, reqs[i].opts);
      }
    });
  }
  for (auto& th : pool) th.join();
  return results;
}

// from_config 伪代码：env AME_DEEPSEEK_API_KEY 优先；否则读 JSON 配置的 api_key；
//   两者皆无返回 nullptr；base_url 固定 DeepSeek，model/cache_dir 由调用方给。
std::unique_ptr<DeepSeekClient> DeepSeekClient::from_config(const std::string& config_path,
                                                            const std::string& model,
                                                            const std::string& cache_dir) {
  std::string key;
  const char* env = std::getenv("AME_DEEPSEEK_API_KEY");
  if (env && *env) {
    key = env;
  } else {
    std::ifstream f(config_path);
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      bool ok = false;
      Json j = Json::parse(ss.str(), &ok);
      if (ok) {
        const Json* k = j.find("api_key");
        if (k) key = k->as_str();
      }
    }
  }
  if (key.empty()) return nullptr;
  return std::make_unique<DeepSeekClient>("https://api.deepseek.com", key, model, cache_dir);
}

// FakeLlmClient::chat 伪代码：记录请求全文；按注册顺序匹配 prompt 子串返回对应响应，
//   都不中返回 default_response。
std::string FakeLlmClient::chat(const std::vector<ChatMessage>& messages,
                                const ChatOptions& opts) {
  (void)opts;
  std::string all;
  for (auto& m : messages) all += m.content + "\n";
  received.push_back(all);
  for (auto& [needle, resp] : rules)
    if (all.find(needle) != std::string::npos) return resp;
  return default_response;
}

}  // namespace ame
