// PgBackend 实现：所有 SQL 检索下推，语义与内存版 Storage 对齐（见头文件注释）。
#include "pg_backend.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <algorithm>

#include "ame/storage/storage.h"  // haversine_m / geohash 工具

namespace ame {
namespace {

const char* kNodeCols =
    "uid,kind,type,name,text,ts,precision,importance,lat,lng,geohash,"
    "status,mention_count,attrs,embedding";
const char* kEdgeCols = "from_uid,to_uid,rel_type,weight,attrs";

std::string i64(long long v) { return std::to_string(v); }
std::string f64(double v) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

// ILIKE 通配转义（%/_/\），配合 ESCAPE '\'
std::string ilike_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '%' || c == '_' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

}  // namespace

PgBackend::~PgBackend() {
  if (conn_) PQfinish(conn_);
}

PgBackend::CallScope::CallScope(const PgBackend* s) : self(s) {
  if (self->depth_++ == 0) {
    ++self->tick_;
    self->evict_if_needed();  // 仅在调用边界换出，调用内部不换出
  }
}
PgBackend::CallScope::~CallScope() { --self->depth_; }

// LRU 换出：缓存到上限时按最近使用序淘汰最旧 10%（仅内存副本，库中数据不动；
// 之后 get_node/query_nodes 未命中自动从库换入）。cap_=0 不限。
void PgBackend::evict_if_needed() const {
  if (cap_ == 0 || cache_.size() < cap_) return;
  size_t target = cap_ - cap_ / 10 - 1;  // 换出约 10%
  std::vector<std::pair<uint64_t, const std::string*>> ord;
  ord.reserve(cache_.size());
  for (auto& kv : cache_) ord.emplace_back(kv.second.used, &kv.first);
  std::sort(ord.begin(), ord.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  size_t n_evict = cache_.size() - target;
  for (size_t i = 0; i < n_evict && i < ord.size(); ++i) cache_.erase(*ord[i].second);
  std::fprintf(stderr, "[pg] 缓存换出 %zu 条（cap=%zu 驻留=%zu，索引常驻=%zu，需要时自动换入）\n",
               n_evict, cap_, cache_.size(), index_.size());
}

// 常驻索引维护见 put_node/clear_ns（uid->kind/name + kind|name->uids）

PGresult* PgBackend::exec(const std::string& sql, const std::vector<std::string>& params) const {
  std::vector<const char*> vals;
  for (auto& p : params) vals.push_back(p.c_str());
  PGresult* r = PQexecParams(conn_, sql.c_str(), (int)params.size(), nullptr, vals.data(),
                             nullptr, nullptr, 0);
  auto st = PQresultStatus(r);
  if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
    std::fprintf(stderr, "[pg] SQL 失败: %s | %s | %.200s\n", PQresStatus(st),
                 PQresultErrorMessage(r), sql.c_str());
    PQclear(r);
    return nullptr;
  }
  return r;
}

Err PgBackend::connect(const std::string& conninfo, const std::string& ns) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (conn_) { PQfinish(conn_); conn_ = nullptr; }
  conn_ = PQconnectdb(conninfo.c_str());
  if (!conn_ || PQstatus(conn_) != CONNECTION_OK) {
    std::fprintf(stderr, "[pg] 连接失败: %s\n", conn_ ? PQerrorMessage(conn_) : "null");
    if (conn_) { PQfinish(conn_); conn_ = nullptr; }
    return Err::IoError;
  }
  ns_ = ns.empty() ? "default" : ns;
  // pgvector 为硬依赖（ann_search 不静默退化全扫）；pg_trgm 仅加速 ILIKE，可缺
  if (!exec("CREATE EXTENSION IF NOT EXISTS vector", {})) {
    std::fprintf(stderr, "[pg] 需要 pgvector 扩展（镜像 pgvector/pgvector:pg16）\n");
    PQfinish(conn_); conn_ = nullptr;
    return Err::Internal;
  }
  if (!exec("CREATE EXTENSION IF NOT EXISTS pg_trgm", {}))
    std::fprintf(stderr, "[pg] pg_trgm 不可用，词法 ILIKE 将走顺序扫描（功能不受影响）\n");
  const char* ddl[] = {
      "CREATE TABLE IF NOT EXISTS ame_nodes("
      "ns text NOT NULL, uid text NOT NULL, kind int NOT NULL, type text NOT NULL DEFAULT '',"
      "name text NOT NULL DEFAULT '', text text NOT NULL DEFAULT '', ts bigint NOT NULL DEFAULT 0,"
      "precision int NOT NULL DEFAULT 3, importance float8 NOT NULL DEFAULT 0.5,"
      "lat float8 NOT NULL DEFAULT 0, lng float8 NOT NULL DEFAULT 0,"
      "geohash text NOT NULL DEFAULT '', status int NOT NULL DEFAULT 0,"
      "mention_count int NOT NULL DEFAULT 0, attrs jsonb NOT NULL DEFAULT '{}',"
      "embedding vector(128), PRIMARY KEY(ns,uid))",
      "CREATE TABLE IF NOT EXISTS ame_edges("
      "ns text NOT NULL, from_uid text NOT NULL, to_uid text NOT NULL, rel_type int NOT NULL,"
      "weight float8 NOT NULL DEFAULT 1, attrs jsonb NOT NULL DEFAULT '{}',"
      "PRIMARY KEY(ns,from_uid,to_uid,rel_type))",
      "CREATE INDEX IF NOT EXISTS ame_nodes_name_idx ON ame_nodes(ns,kind,name)",
      "CREATE INDEX IF NOT EXISTS ame_nodes_kind_idx ON ame_nodes(ns,kind)",
      "CREATE INDEX IF NOT EXISTS ame_nodes_attrs_gin ON ame_nodes USING gin(attrs)",
      "CREATE INDEX IF NOT EXISTS ame_nodes_emb_hnsw ON ame_nodes USING hnsw(embedding vector_cosine_ops)",
      "CREATE INDEX IF NOT EXISTS ame_edges_from_idx ON ame_edges(ns,from_uid)",
      "CREATE INDEX IF NOT EXISTS ame_edges_to_idx ON ame_edges(ns,to_uid)",
      "CREATE INDEX IF NOT EXISTS ame_nodes_text_trgm ON ame_nodes USING gin(text gin_trgm_ops)",
  };
  for (auto* sql : ddl) {
    if (!exec(sql, {})) {
      // trgm 索引失败不致命（扩展缺失时已警告），其余失败视为连接不可用
      if (std::strstr(sql, "text_trgm")) continue;
      PQfinish(conn_); conn_ = nullptr;
      return Err::Internal;
    }
  }
  // 数据缓存上限：env AME_PG_CACHE_NODES（默认 50000，0=不限）；索引常驻全量装载
  if (const char* cap = std::getenv("AME_PG_CACHE_NODES")) {
    long v = std::atol(cap);
    if (v >= 0) cap_ = (size_t)v;
  }
  PGresult* ri = exec("SELECT uid,kind,name FROM ame_nodes WHERE ns=$1", {ns_});
  if (!ri) { PQfinish(conn_); conn_ = nullptr; return Err::IoError; }
  for (int i = 0; i < PQntuples(ri); ++i) {
    std::string uid = PQgetvalue(ri, i, 0);
    IndexEntry ie;
    ie.kind = (NodeKind)std::atoi(PQgetvalue(ri, i, 1));
    ie.name = PQgetvalue(ri, i, 2);
    index_[uid] = ie;
    name_index_[i64((int)ie.kind) + "|" + ie.name].push_back(uid);
  }
  PQclear(ri);
  std::fprintf(stderr, "[pg] 已连接 ns=%s：索引常驻 %zu 条，数据缓存上限 %zu（LRU 换入换出）\n",
               ns_.c_str(), index_.size(), cap_);
  return Err::Ok;
}

std::string PgBackend::vec_literal(const Vec& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.9g", (double)v[i]);
    s += buf;
  }
  return s + "]";
}

Vec PgBackend::parse_vec(const char* s) {
  Vec v;
  if (!s) return v;
  const char* p = s;
  while (*p && *p != '[') ++p;
  if (*p == '[') ++p;
  while (*p && *p != ']') {
    char* end = nullptr;
    double d = std::strtod(p, &end);
    if (end == p) break;
    v.push_back((float)d);
    p = end;
    if (*p == ',') ++p;
  }
  return v;
}

Json PgBackend::attrs_to_json(const Attrs& a) {
  Json j = Json::object();
  for (auto& kv : a) j[kv.first] = Json::string(kv.second);
  return j;
}

Attrs PgBackend::attrs_from_json(const char* s) {
  Attrs a;
  bool ok = false;
  Json j = Json::parse(s ? s : "{}", &ok);
  if (ok)
    for (auto& kv : j.obj) a[kv.first] = kv.second.as_str();
  return a;
}

Node PgBackend::row_to_node(PGresult* r, int row) const {
  Node n;
  n.uid = PQgetvalue(r, row, 0);
  n.kind = (NodeKind)std::atoi(PQgetvalue(r, row, 1));
  n.type = PQgetvalue(r, row, 2);
  n.name = PQgetvalue(r, row, 3);
  n.text = PQgetvalue(r, row, 4);
  n.ts = (TimePoint)std::atoll(PQgetvalue(r, row, 5));
  n.precision = (Precision)std::atoi(PQgetvalue(r, row, 6));
  n.importance = std::atof(PQgetvalue(r, row, 7));
  n.lat = std::atof(PQgetvalue(r, row, 8));
  n.lng = std::atof(PQgetvalue(r, row, 9));
  n.geohash = PQgetvalue(r, row, 10);
  n.status = std::atoi(PQgetvalue(r, row, 11));
  n.mention_count = std::atoi(PQgetvalue(r, row, 12));
  n.attrs = attrs_from_json(PQgetvalue(r, row, 13));
  if (!PQgetisnull(r, row, 14)) n.embedding = parse_vec(PQgetvalue(r, row, 14));
  return n;
}

Edge PgBackend::row_to_edge(PGresult* r, int row) const {
  Edge e;
  e.from = PQgetvalue(r, row, 0);
  e.to = PQgetvalue(r, row, 1);
  e.type = (RelType)std::atoi(PQgetvalue(r, row, 2));
  e.weight = std::atof(PQgetvalue(r, row, 3));
  e.attrs = attrs_from_json(PQgetvalue(r, row, 4));
  return e;
}

const Node* PgBackend::materialize(Node n) const {
  auto& slot = cache_[n.uid];
  slot.used = tick_;
  if (slot.node) {
    *slot.node = std::move(n);  // 就地更新，已外发指针不失效
  } else {
    slot.node = std::make_unique<Node>(std::move(n));  // 未命中换入
  }
  return slot.node.get();
}

std::vector<const Node*> PgBackend::query_nodes(const std::string& sql,
                                                const std::vector<std::string>& params) const {
  std::vector<const Node*> out;
  PGresult* r = exec(sql, params);
  if (!r) return out;
  int n = PQntuples(r);
  out.reserve(n);
  for (int i = 0; i < n; ++i) out.push_back(materialize(row_to_node(r, i)));
  PQclear(r);
  return out;
}

Err PgBackend::put_node(const Node& n) {
  if (n.uid.empty()) return Err::InvalidArg;
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  const char* sql =
      "INSERT INTO ame_nodes(ns,uid,kind,type,name,text,ts,precision,importance,lat,lng,"
      "geohash,status,mention_count,attrs,embedding) VALUES("
      "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15::jsonb,NULLIF($16,'')::vector) "
      "ON CONFLICT(ns,uid) DO UPDATE SET kind=EXCLUDED.kind,type=EXCLUDED.type,"
      "name=EXCLUDED.name,text=EXCLUDED.text,ts=EXCLUDED.ts,precision=EXCLUDED.precision,"
      "importance=EXCLUDED.importance,lat=EXCLUDED.lat,lng=EXCLUDED.lng,"
      "geohash=EXCLUDED.geohash,status=EXCLUDED.status,mention_count=EXCLUDED.mention_count,"
      "attrs=EXCLUDED.attrs,embedding=EXCLUDED.embedding";
  std::vector<std::string> p = {
      ns_, n.uid, i64((int)n.kind), n.type, n.name, n.text, i64(n.ts), i64((int)n.precision),
      f64(n.importance), f64(n.lat), f64(n.lng), n.geohash, i64(n.status),
      i64(n.mention_count), attrs_to_json(n.attrs).dump(),
      n.embedding.empty() ? "" : vec_literal(n.embedding)};
  PGresult* r = exec(sql, p);
  if (!r) return Err::IoError;
  PQclear(r);
  materialize(n);  // 写穿同步缓存
  {  // 常驻索引同步（kind/name 变更时摘除旧名字项）
    auto old = index_.find(n.uid);
    if (old != index_.end() && (old->second.kind != n.kind || old->second.name != n.name)) {
      std::string okey = i64((int)old->second.kind) + "|" + old->second.name;
      auto& ov = name_index_[okey];
      ov.erase(std::remove(ov.begin(), ov.end(), n.uid), ov.end());
    }
    index_[n.uid] = IndexEntry{n.kind, n.name};
    auto& v = name_index_[i64((int)n.kind) + "|" + n.name];
    if (std::find(v.begin(), v.end(), n.uid) == v.end()) v.push_back(n.uid);
  }
  return Err::Ok;
}

Err PgBackend::upsert_edge(const Edge& e, bool add_weight) {
  if (e.from.empty() || e.to.empty()) return Err::InvalidArg;
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  // 与内存版一致：两端节点必须存在（常驻索引 O(1)，免两次 SQL）
  if (!index_.count(e.from) || !index_.count(e.to)) return Err::NotFound;
  const char* sql =
      "INSERT INTO ame_edges(ns,from_uid,to_uid,rel_type,weight,attrs) "
      "VALUES($1,$2,$3,$4,$5,$6::jsonb) "
      "ON CONFLICT(ns,from_uid,to_uid,rel_type) DO UPDATE SET "
      "weight=CASE WHEN $7::boolean THEN ame_edges.weight+EXCLUDED.weight ELSE EXCLUDED.weight END,"
      "attrs=ame_edges.attrs||EXCLUDED.attrs";
  PGresult* r = exec(sql, {ns_, e.from, e.to, i64((int)e.type), f64(e.weight),
                           attrs_to_json(e.attrs).dump(), add_weight ? "true" : "false"});
  if (!r) return Err::IoError;
  PQclear(r);
  return Err::Ok;
}

const Edge* PgBackend::find_edge(const std::string& from, const std::string& to, RelType t) const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  PGresult* r = exec(
      std::string("SELECT ") + kEdgeCols +
          " FROM ame_edges WHERE ns=$1 AND from_uid=$2 AND to_uid=$3 AND rel_type=$4 LIMIT 1",
      {ns_, from, to, i64((int)t)});
  if (!r) return nullptr;
  const Edge* out = nullptr;
  if (PQntuples(r) > 0) {
    edge_cache_ = std::make_unique<Edge>(row_to_edge(r, 0));
    out = edge_cache_.get();
  }
  PQclear(r);
  return out;
}

const Node* PgBackend::get_node(const std::string& uid) const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  auto it = cache_.find(uid);
  if (it != cache_.end()) {
    it->second.used = tick_;  // 命中只更新 LRU，不打 SQL
    return it->second.node.get();
  }
  PGresult* r = exec(std::string("SELECT ") + kNodeCols +
                         " FROM ame_nodes WHERE ns=$1 AND uid=$2 LIMIT 1",
                     {ns_, uid});
  if (!r) return nullptr;
  const Node* out = PQntuples(r) > 0 ? materialize(row_to_node(r, 0)) : nullptr;
  PQclear(r);
  return out;
}

Node* PgBackend::get_node_mut(const std::string& uid) {
  // 可变指针语义同内存版；调用方修改后必须 put_node 写回（4.0 已在全部调用点补齐）
  return const_cast<Node*>(get_node(uid));
}

std::vector<std::pair<Edge, const Node*>> PgBackend::neighbors(
    const std::string& uid, const std::vector<RelType>* rel_types) const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  std::vector<std::pair<Edge, const Node*>> res;
  auto want = [&](RelType t) {
    if (!rel_types) return true;
    for (auto x : *rel_types)
      if (x == t) return true;
    return false;
  };
  auto push = [&](const Edge& e, const std::string& other) {
    if (!want(e.type)) return;
    const Node* n = get_node(other);
    if (n) res.emplace_back(e, n);
  };
  PGresult* ro = exec(std::string("SELECT ") + kEdgeCols + " FROM ame_edges WHERE ns=$1 AND from_uid=$2",
                      {ns_, uid});
  if (ro) {
    for (int i = 0; i < PQntuples(ro); ++i) {
      Edge e = row_to_edge(ro, i);
      push(e, e.to);
    }
    PQclear(ro);
  }
  PGresult* ri = exec(std::string("SELECT ") + kEdgeCols + " FROM ame_edges WHERE ns=$1 AND to_uid=$2",
                      {ns_, uid});
  if (ri) {
    for (int i = 0; i < PQntuples(ri); ++i) {
      Edge e = row_to_edge(ri, i);
      std::string other = e.from;
      std::swap(e.from, e.to);  // 入边反向视角：uid -> other
      e.attrs["_reversed"] = "1";
      push(e, other);
    }
    PQclear(ri);
  }
  return res;
}

std::vector<std::pair<std::string, double>> PgBackend::ann_search(const Vec& embedding,
                                                                  int topk) const {
  std::vector<std::pair<std::string, double>> out;
  if (embedding.empty() || topk <= 0) return out;
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  std::string lit = vec_literal(embedding);
  PGresult* r = exec(
      "SELECT uid, 1-(embedding <=> $2::vector) AS sim FROM ame_nodes "
      "WHERE ns=$1 AND embedding IS NOT NULL "
      "ORDER BY embedding <=> $2::vector ASC LIMIT $3",
      {ns_, lit, i64(topk)});
  if (!r) return out;
  for (int i = 0; i < PQntuples(r); ++i) {
    double sim = std::atof(PQgetvalue(r, i, 1));
    if (sim > 0) out.emplace_back(PQgetvalue(r, i, 0), sim);
  }
  PQclear(r);
  return out;
}

std::vector<const Node*> PgBackend::geo_lookup(double lat, double lng, double radius_m) const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  // Place 量级小：按 kind 取出后 C++ haversine 过滤（R-tree/PostGIS 留后续）
  auto places = query_nodes(std::string("SELECT ") + kNodeCols +
                                " FROM ame_nodes WHERE ns=$1 AND kind=$2",
                            {ns_, i64((int)NodeKind::Place)});
  std::vector<const Node*> out;
  for (auto* n : places)
    if (haversine_m(lat, lng, n->lat, n->lng) <= radius_m) out.push_back(n);
  std::sort(out.begin(), out.end(), [&](const Node* a, const Node* b) {
    return haversine_m(lat, lng, a->lat, a->lng) < haversine_m(lat, lng, b->lat, b->lng);
  });
  return out;
}

const Node* PgBackend::find_by_name(NodeKind kind, const std::string& name) const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  // 常驻名字索引免 SQL；节点数据未命中时按需换入
  auto it = name_index_.find(i64((int)kind) + "|" + name);
  if (it == name_index_.end() || it->second.empty()) return nullptr;
  return get_node(it->second.front());
}

std::vector<const Node*> PgBackend::find_all_by_name(NodeKind kind,
                                                     const std::string& name) const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  std::vector<const Node*> out;
  auto it = name_index_.find(i64((int)kind) + "|" + name);
  if (it == name_index_.end()) return out;
  for (auto& uid : it->second)
    if (const Node* n = get_node(uid)) out.push_back(n);
  return out;
}

const Node* PgBackend::find_by_attr(NodeKind kind, const std::string& key,
                                    const std::string& val) const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  // JSONB 包含查询（GIN 可索引）：attrs @> {"key":"val"}
  Json cond = Json::object();
  cond[key] = Json::string(val);
  auto v = query_nodes(std::string("SELECT ") + kNodeCols +
                           " FROM ame_nodes WHERE ns=$1 AND kind=$2 AND attrs @> $3::jsonb LIMIT 1",
                       {ns_, i64((int)kind), cond.dump()});
  return v.empty() ? nullptr : v[0];
}

std::vector<const Node*> PgBackend::nodes_of_kind(NodeKind k) const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  return query_nodes(std::string("SELECT ") + kNodeCols + " FROM ame_nodes WHERE ns=$1 AND kind=$2",
                     {ns_, i64((int)k)});
}

std::vector<const Node*> PgBackend::lexical_event_candidates(
    const std::vector<std::string>& words, int limit) const {
  if (words.empty() || limit <= 0) return {};
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  std::string sql = std::string("SELECT ") + kNodeCols +
                    " FROM ame_nodes WHERE ns=$1 AND kind=" + i64((int)NodeKind::Event) +
                    " AND (text ILIKE '%'||$2||'%'";
  std::vector<std::string> p = {ns_};
  for (size_t i = 0; i < words.size(); ++i) {
    p.push_back(ilike_escape(words[i]));
    if (i > 0) sql += " OR text ILIKE '%'||$" + i64((long long)(i + 2)) + "||'%'";
  }
  sql += ") LIMIT $" + i64((long long)(words.size() + 2));  // LIKE 默认 ESCAPE 即反斜杠
  p.push_back(i64(limit));
  return query_nodes(sql, p);
}

size_t PgBackend::node_count() const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  PGresult* r = exec("SELECT count(*) FROM ame_nodes WHERE ns=$1", {ns_});
  if (!r) return 0;
  size_t n = (size_t)std::atoll(PQgetvalue(r, 0, 0));
  PQclear(r);
  return n;
}

size_t PgBackend::edge_count() const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  PGresult* r = exec("SELECT count(*) FROM ame_edges WHERE ns=$1", {ns_});
  if (!r) return 0;
  size_t n = (size_t)std::atoll(PQgetvalue(r, 0, 0));
  PQclear(r);
  return n;
}

Err PgBackend::clear_ns() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  PGresult* r = exec("DELETE FROM ame_edges WHERE ns=$1", {ns_});
  if (!r) return Err::IoError;
  PQclear(r);
  r = exec("DELETE FROM ame_nodes WHERE ns=$1", {ns_});
  if (!r) return Err::IoError;
  PQclear(r);
  cache_.clear();
  index_.clear();
  name_index_.clear();
  return Err::Ok;
}

std::vector<Node> PgBackend::all_nodes() const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  std::vector<Node> out;
  PGresult* r = exec(std::string("SELECT ") + kNodeCols + " FROM ame_nodes WHERE ns=$1", {ns_});
  if (!r) return out;
  out.reserve(PQntuples(r));
  for (int i = 0; i < PQntuples(r); ++i) out.push_back(row_to_node(r, i));
  PQclear(r);
  return out;
}

std::vector<Edge> PgBackend::all_edges() const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  CallScope cs(this);
  std::vector<Edge> out;
  PGresult* r = exec(std::string("SELECT ") + kEdgeCols + " FROM ame_edges WHERE ns=$1", {ns_});
  if (!r) return out;
  out.reserve(PQntuples(r));
  for (int i = 0; i < PQntuples(r); ++i) out.push_back(row_to_edge(r, i));
  PQclear(r);
  return out;
}

}  // namespace ame
