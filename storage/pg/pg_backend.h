/*
 * 所属模块：M9 存储引擎——PostgreSQL 后端（4.0 新增，编译开关 AME_WITH_POSTGRES）。
 * 本文件思路：PgBackend 与内存版 Storage 同语义实现全部接口，Storage 公开方法
 *   在 pg_ 激活时委托到此处；节点/边规范化存 ame_nodes/ame_edges 两表，
 *   检索（名字/属性/向量/邻居/词法候选）全部下推 SQL，不做整图内存化。
 * 关键算法/数据结构：
 *   - 读穿缓存 cache_（uid -> unique_ptr<Node>）为接口的 const Node* 返回提供
 *     稳定指针；写时 put_node 就地更新缓存值（指针不失效）；P1 不淘汰
 *     （内存随触碰工作集增长，见 storage/pg/README.md 限制说明）。
 *   - ann_search 走 pgvector <=> 余弦距离 ORDER BY LIMIT（HNSW）；
 *   - 所有表带 ns 命名空间列（session 隔离），主键/索引均含 ns。
 * 依赖关系：libpq（PostgreSQL C API）、core(types/json)；仅被 storage.cpp 包含。
 */
#pragma once

#include <libpq-fe.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ame/core/json.h"
#include "ame/core/types.h"

namespace ame {

class PgBackend {
 public:
  PgBackend() = default;
  ~PgBackend();
  PgBackend(const PgBackend&) = delete;
  PgBackend& operator=(const PgBackend&) = delete;

  // 连接并幂等建表/索引（pgvector/pg_trgm 扩展缺失返回 Internal 并记日志）
  Err connect(const std::string& conninfo, const std::string& ns);
  bool active() const { return conn_ != nullptr; }
  const std::string& ns() const { return ns_; }

  Err put_node(const Node& n);
  Err upsert_edge(const Edge& e, bool add_weight);
  const Edge* find_edge(const std::string& from, const std::string& to, RelType t) const;
  const Node* get_node(const std::string& uid) const;
  Node* get_node_mut(const std::string& uid);
  std::vector<std::pair<Edge, const Node*>> neighbors(
      const std::string& uid, const std::vector<RelType>* rel_types) const;
  std::vector<std::pair<std::string, double>> ann_search(const Vec& embedding, int topk) const;
  std::vector<const Node*> geo_lookup(double lat, double lng, double radius_m) const;
  const Node* find_by_name(NodeKind kind, const std::string& name) const;
  std::vector<const Node*> find_all_by_name(NodeKind kind, const std::string& name) const;
  const Node* find_by_attr(NodeKind kind, const std::string& key, const std::string& val) const;
  std::vector<const Node*> nodes_of_kind(NodeKind k) const;
  // 词法事件候选下推：words 为调用方已切好的内容词（ASCII 或中文均可），
  // OR ILIKE 预过滤 + LIMIT，避免 Event 全表拉取；打分仍在 seed 侧原逻辑完成
  std::vector<const Node*> lexical_event_candidates(const std::vector<std::string>& words,
                                                    int limit) const;
  size_t node_count() const;
  size_t edge_count() const;
  size_t cache_size() const { return cache_.size(); }   // 数据缓存驻留节点数（诊断/测试）
  size_t index_size() const { return index_.size(); }   // 常驻索引条目数（诊断/测试）

  // 快照导出用全量读取（大库慎用，仅供备份/迁移）
  std::vector<Node> all_nodes() const;
  std::vector<Edge> all_edges() const;
  // 清空本 ns 全部数据与缓存（from_json 替换式载入/删 session 语义）
  Err clear_ns();

 private:
  // 调用边界守卫：最外层公开调用开始时 tick_++ 并触发 LRU 换出；
  // 调用内部不换出，保证本次调用返回的 Node* 在调用期间绝不被淘汰
  struct CallScope {
    const PgBackend* self;
    explicit CallScope(const PgBackend* s);
    ~CallScope();
  };
  void evict_if_needed() const;  // LRU：超上限时最久未用的一批换出（库中有全量数据）

  // 以下 helper 均不持锁，公开方法入口统一 lock_guard(mu_)
  PGresult* exec(const std::string& sql, const std::vector<std::string>& params) const;
  Node row_to_node(PGresult* r, int row) const;
  Edge row_to_edge(PGresult* r, int row) const;
  // 读穿物化：已缓存直接返回（put_node 负责写时同步），否则插入新副本（换入）
  const Node* materialize(Node n) const;
  std::vector<const Node*> query_nodes(const std::string& sql,
                                       const std::vector<std::string>& params) const;
  static std::string vec_literal(const Vec& v);        // -> '[f1,f2,...]'
  static Vec parse_vec(const char* s);                  // pgvector 文本 '[f1,...]' -> Vec
  static Json attrs_to_json(const Attrs& a);
  static Attrs attrs_from_json(const char* s);

  struct CacheEntry {
    std::unique_ptr<Node> node;
    uint64_t used = 0;  // 最近使用 tick（LRU）
  };
  struct IndexEntry {
    NodeKind kind = NodeKind::Event;
    std::string name;  // 轻量常驻索引：uid -> kind/name（连接时全量装载，写时维护）
  };

  PGconn* conn_ = nullptr;
  std::string ns_;
  size_t cap_ = 50000;                 // 数据缓存节点上限（AME_PG_CACHE_NODES，0=不限）
  mutable uint64_t tick_ = 0;
  mutable int depth_ = 0;              // CallScope 嵌套深度（仅最外层触发换出）
  mutable std::recursive_mutex mu_;
  mutable std::unordered_map<std::string, CacheEntry> cache_;
  mutable std::unordered_map<std::string, IndexEntry> index_;
  mutable std::unordered_map<std::string, std::vector<std::string>> name_index_;  // "kind|name"->uids
  mutable std::unique_ptr<Edge> edge_cache_;  // find_edge 返回指针的持有处（每次调用覆写）
};

}  // namespace ame
