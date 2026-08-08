// =============================================================================
// 模块定位：M9 存储引擎 —— 联想记忆引擎的图存储层，负责节点/边的内存存取、
//           邻居查询、向量近邻检索、空间查询与 JSON 快照持久化。
//
// 本文件思路：对外声明 Storage 类的全部接口及 geohash/haversine 两个工具函数。
//           存储采用"节点表 + 出边/入边双邻接表"结构，读写通过共享互斥锁
//           保证线程安全；接口语义（upsert 合并、反向视角邻居等）在声明处注释。
//
// 关键算法/数据结构：
//   - 哈希表 nodes_（uid -> Node）+ out_/in_ 邻接表（uid -> Edge 列表），
//     边同时登记在两端，便于正反两向遍历；
//   - ann_search 为暴力余弦 topK（生产可替换为 hnswlib，接口不变）；
//   - geo_lookup 为暴力 haversine 距离扫描（生产可替换为 GeoHash/R-tree）；
//   - geohash_encode 提供标准 base32 geohash 编码，供 Region 按前缀挂载。
//
// 依赖关系：仅依赖 core 公共层 ame/core/types.h（Node/Edge/NodeKind/RelType/
//           Vec/Err 等基础类型）。本模块被 M4 graph_writer、M6 diffusion、
//           M7 rerank、M8 thinker 等所有需要读写记忆图的模块依赖。
// =============================================================================
#pragma once
#include "ame/core/types.h"
#include "ame/core/json.h"

#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace ame {

// geohash 编码（base32，供 Region 按前缀自动挂载）
std::string geohash_encode(double lat, double lng, int precision = 6);
double haversine_m(double lat1, double lng1, double lat2, double lng2);

class Storage {
 public:
  // upsert 节点；uid 为空则报错
  Err put_node(const Node& n);
  // 写入边；同 (from,to,type) 已存在则累加/更新（见 merge_edge）
  Err put_edge(const Edge& e);
  // 同 (from,to,type) 边存在则取出
  const Edge* find_edge(const std::string& from, const std::string& to, RelType t) const;
  // 更新已存在边的 weight 与 attrs（不存在则新建）
  Err upsert_edge(const Edge& e, bool add_weight);

  const Node* get_node(const std::string& uid) const;
  Node* get_node_mut(const std::string& uid);

  // 邻居：出边 + 入边（入边以反向视角返回，edge.from==邻居 uid）。
  // rel_types 为空则全部类型。
  std::vector<std::pair<Edge, const Node*>> neighbors(
      const std::string& uid, const std::vector<RelType>* rel_types = nullptr) const;
  // 伪代码：initializer_list 便捷重载
  //   步骤1：把初始化列表拷入 vector；
  //   步骤2：转调主版本 neighbors(uid, &v)。
  std::vector<std::pair<Edge, const Node*>> neighbors(
      const std::string& uid, std::initializer_list<RelType> rel_types) const {
    std::vector<RelType> v(rel_types);
    return neighbors(uid, &v);
  }

  // 暴力余弦 topK（仅扫描 embedding 非空的节点）。返回 (uid, similarity)
  std::vector<std::pair<std::string, double>> ann_search(const Vec& embedding, int topk) const;

  // 暴力距离查询 Place 节点，radius 单位米
  std::vector<const Node*> geo_lookup(double lat, double lng, double radius_m) const;

  // 按 kind+name 精确查找（名字索引 O(1)；同名多节点时返回首个，语义同全表扫描版）
  const Node* find_by_name(NodeKind kind, const std::string& name) const;
  // 按 kind+name 查找全部同名节点（消歧候选枚举用）
  std::vector<const Node*> find_all_by_name(NodeKind kind, const std::string& name) const;
  // 按 attr 键值查找（强标识符反查）
  const Node* find_by_attr(NodeKind kind, const std::string& key, const std::string& val) const;

  std::vector<const Node*> nodes_of_kind(NodeKind k) const;
  // 返回节点总数（trivial getter）
  size_t node_count() const { return nodes_.size(); }
  size_t edge_count() const;

  // JSON 快照
  Err save(const std::string& path) const;
  Err load(const std::string& path);
  // 内存版序列化/反序列化（供 M12 组合快照：图 + 实体档案槽位）
  Json to_json() const;
  Err from_json(const Json& root);

  static double cosine(const Vec& a, const Vec& b);

 private:
  std::unordered_map<std::string, Node> nodes_;
  std::unordered_map<std::string, std::vector<Edge>> out_;  // uid -> 出边
  std::unordered_map<std::string, std::vector<Edge>> in_;   // uid -> 入边
  // 名字索引："kind|name" -> 同名节点 uid 列表（put_node/load 时维护，无删除语义）
  std::unordered_map<std::string, std::vector<std::string>> name_index_;
  mutable std::shared_mutex mu_;
};

}  // namespace ame
