/*
 * 所属模块：M13 向量服务（vector）—— 负责文本/节点的向量表示计算、缓存与组合运算。
 * 本文件主体思路：声明向量服务的对外接口。IEmbedder 为可插拔的 embedding 接口，
 *   HashEmbedder 是默认实现（无 LLM/无外部模型的确定性哈希 embedding，生产可替换为
 *   BGE 等真实模型）；VectorService 是门面类，提供带缓存的 embed、节点向量、
 *   向量组合、边向量差分与成分分解。
 * 关键算法/数据结构：字符 n-gram(n=1,2,3) feature hashing 到 128 维并 L2 归一化；
 *   嵌入缓存采用 unordered_map + mutex 保证线程安全；接口层本身无复杂算法。
 * 依赖关系：依赖 core 公共层的类型定义（ame/core/types.h 的 Vec/Node）；
 *   前置声明 storage(M9) 的 Storage，用于 decompose 的近邻检索（实现在 .cpp）；
 *   被 thinker(M8)/engine(M12) 等上层模块作为向量能力入口调用。
 */
#pragma once
#include "ame/core/types.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace ame {

class Storage;

// 可插拔的 embedding 接口：由调用方注入实现，默认回退到 HashEmbedder
class IEmbedder {
 public:
  virtual ~IEmbedder() = default;
  virtual Vec embed_text(const std::string& text) = 0;
  virtual int dim() const = 0;
};

// 确定性哈希 embedding（支持中文 UTF-8）
class HashEmbedder : public IEmbedder {
 public:
  static constexpr int kDim = 128;
  Vec embed_text(const std::string& text) override;
  int dim() const override { return kDim; }  // 固定 128 维
};

class VectorService {
 public:
  // storage 用于 ann_search 取近邻（decompose），可为空
  explicit VectorService(const Storage* storage = nullptr,
                         IEmbedder* embedder = nullptr);

  Vec embed(const std::string& text);        // 带缓存
  Vec embed_node(const Node& node);          // 按 name+text+type 计算
  Vec compose(const std::vector<Vec>& vectors, const std::string& op = "add");
  Vec edge_vec(const Vec& a, const Vec& b);  // emb(B) - emb(A)
  // 成分分解（简化版）：返回与最近邻节点的方向分量 [(neighbor_uid, direction, weight)]
  std::vector<std::pair<std::string, double>> decompose(const std::string& uid, int topk = 8);

  static void l2_normalize(Vec& v);

 private:
  const Storage* storage_;
  HashEmbedder default_embedder_;
  IEmbedder* embedder_;
  std::unordered_map<std::string, Vec> cache_;
  std::mutex mu_;
};

}  // namespace ame
