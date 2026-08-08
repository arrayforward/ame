/*
 * 所属模块：llm 接入层（无 M 编号，工程附加层）——DeepSeek LLM 客户端统一接口。
 * 本文件主体思路：定义 ILlmClient 抽象接口（chat 单轮对话）与 DeepSeekClient 实现；
 *   HTTP 传输走 curl 子进程（零第三方依赖约束下的折中，Transport 可注入替换——
 *   生产可换 libcurl，测试注入 fake transport，单元测试不打真实 API）；
 *   磁盘缓存（请求哈希 → 响应）保证断点续跑不重复扣费。
 * 关键算法/数据结构：缓存键 = FNV-1a(模型+消息+选项) 的 64 位哈希文件名；
 *   并发用原子索引 + 固定线程池；重试为指数退避（429/5xx，最多 5 次）。
 * 安全约束：API key 从 bench/config/deepseek.local.json 读取，
 *   环境变量 AME_DEEPSEEK_API_KEY 优先；key 不出现在任何日志/输出中。
 * 依赖关系：依赖 core(json.h/types.h)；被 llm 组件层（components.h）与 bench 依赖。
 */
#pragma once
#include "ame/core/types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ame {

struct ChatMessage {
  std::string role;     // system|user|assistant
  std::string content;
};

struct ChatOptions {
  double temperature = 0.0;
  int max_tokens = 1024;
  bool json = false;                  // response_format=json_object
  bool disable_thinking = true;       // deepseek-v4 推理模型：默认关思考
                                      //（thinking:{type:disabled}），避免 reasoning 烧光预算
};

struct ChatRequest {
  std::vector<ChatMessage> messages;
  ChatOptions opts;
};

class ILlmClient {
 public:
  virtual ~ILlmClient() = default;
  // 单轮对话；失败（网络/解析）返回空串
  virtual std::string chat(const std::vector<ChatMessage>& messages,
                           const ChatOptions& opts) = 0;
  // 批量：默认顺序逐条（子类可覆盖为并发实现）
  virtual std::vector<std::string> chat_batch(const std::vector<ChatRequest>& reqs,
                                              int concurrency = 8) {
    (void)concurrency;
    std::vector<std::string> out;
    for (auto& r : reqs) out.push_back(chat(r.messages, r.opts));
    return out;
  }
};

// 传输层抽象：输入请求体 JSON，输出 HTTP 状态码（0=传输失败）与响应体
using LlmTransport =
    std::function<int(const std::string& body, std::string* out)>;

class DeepSeekClient : public ILlmClient {
 public:
  // base_url 形如 https://api.deepseek.com（端点 /chat/completions 在实现内拼接）
  DeepSeekClient(std::string base_url, std::string api_key, std::string model,
                 std::string cache_dir);

  std::string chat(const std::vector<ChatMessage>& messages,
                   const ChatOptions& opts) override;

  // 批量并发：固定线程池 + 原子索引分派，结果与请求同序
  std::vector<std::string> chat_batch(const std::vector<ChatRequest>& reqs,
                                      int concurrency = 8) override;

  // 测试注入 fake transport（默认 curl 子进程）
  void set_transport(LlmTransport t) { transport_ = std::move(t); }

  // 统计（测试/成本核算用）
  int api_calls() const { return api_calls_; }
  int cache_hits() const { return cache_hits_; }

  // 从配置文件构造：env AME_DEEPSEEK_API_KEY 优先，其次 JSON 配置 {"api_key":...}
  // 配置缺失返回 nullptr
  static std::unique_ptr<DeepSeekClient> from_config(const std::string& config_path,
                                                     const std::string& model,
                                                     const std::string& cache_dir);

 private:
  std::string cache_key(const std::vector<ChatMessage>& messages,
                        const ChatOptions& opts) const;
  std::string call_api(const std::vector<ChatMessage>& messages, ChatOptions opts);  // 含重试
  std::string build_body(const std::vector<ChatMessage>& messages,
                         const ChatOptions& opts) const;

  std::string base_url_, api_key_, model_, cache_dir_;
  LlmTransport transport_;
  int api_calls_ = 0;
  int cache_hits_ = 0;
};

// 测试用假客户端：按注册顺序或规则返回固定响应，记录收到的请求
class FakeLlmClient : public ILlmClient {
 public:
  std::string default_response;
  std::vector<std::pair<std::string, std::string>> rules;  // prompt 子串 -> 响应
  std::vector<std::string> received;

  std::string chat(const std::vector<ChatMessage>& messages,
                   const ChatOptions& opts) override;
};

}  // namespace ame
