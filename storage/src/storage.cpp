// =============================================================================
// 模块定位：M9 存储引擎 —— 联想记忆引擎的图存储层，实现节点/边存取、邻居遍历、
//           向量检索、空间查询与 JSON 快照。
//
// 本文件思路：实现 storage.h 声明的全部接口。图数据保存在内存哈希表与双邻接
//           表中，所有公共入口先取 shared_mutex（读共享/写独占）再操作；
//           快照以 JSON 文本整体导出/载入，载入时先清空再重建两张邻接表。
//
// 关键算法/数据结构：
//   - geohash_encode：经纬度区间逐位二分（奇偶位交替取经/纬度），5 位一组
//     映射 base32 字符；
//   - haversine_m：球面大圆距离公式，用于 Place 节点的半径过滤与排序；
//   - ann_search：暴力全表余弦相似度 + partial_sort 取 topK（O(N)，可替换
//     为 hnswlib 等 ANN 索引）；
//   - upsert_edge：按 (from,to,type) 去重合并，同键边累加或覆盖权重并合并
//     attrs，保证出/入两张邻接表同步。
//
// 依赖关系：依赖 core 公共层 types.h（Node/Edge/RelType/Err 等）与 json.h
//           （Json 序列化/解析，供 save/load 使用）；不依赖其它业务模块。
// =============================================================================
#include "ame/storage/storage.h"

#include "ame/core/json.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>

namespace ame {

// ---------------- geohash / haversine ----------------
static const char* kGh = "0123456789bcdefghjkmnpqrstuvwxyz";

// 伪代码：标准 geohash 编码（base32）
//   步骤1：初始化纬度区间 [-90,90]、经度区间 [-180,180]，位计数 bit=0；
//   步骤2：循环直到凑满 precision 个字符——偶数位（even）二分经度、奇数位
//          二分纬度：坐标 >= 区间中点则当前位置 1 并抬升下界，否则置 0
//          并压低上界；
//   步骤3：每攒满 5 位，查 base32 表 kGh 追加一个字符并重置 bit/ch。
std::string geohash_encode(double lat, double lng, int precision) {
  double la[2] = {-90, 90}, lo[2] = {-180, 180};
  std::string s;
  bool even = true;
  int bit = 0, ch = 0;
  while ((int)s.size() < precision) {
    if (even) {
      double mid = (lo[0] + lo[1]) / 2;
      if (lng >= mid) { ch |= 1 << (4 - bit); lo[0] = mid; }
      else lo[1] = mid;
    } else {
      double mid = (la[0] + la[1]) / 2;
      if (lat >= mid) { ch |= 1 << (4 - bit); la[0] = mid; }
      else la[1] = mid;
    }
    even = !even;
    if (bit < 4) ++bit;
    else { s += kGh[ch]; bit = 0; ch = 0; }
  }
  return s;
}

// 伪代码：haversine 球面大圆距离（单位米）
//   步骤1：分别取纬度差、经度差半角的正弦 a、b；
//   步骤2：h = a^2 + cos(lat1)*cos(lat2)*b^2；
//   步骤3：返回 2R*asin(sqrt(h))，R 取地球半径 6371000 米。
double haversine_m(double lat1, double lng1, double lat2, double lng2) {
  const double R = 6371000.0, D2R = M_PI / 180.0;
  double a = std::sin((lat2 - lat1) * D2R / 2);
  double b = std::sin((lng2 - lng1) * D2R / 2);
  double h = a * a + std::cos(lat1 * D2R) * std::cos(lat2 * D2R) * b * b;
  return 2 * R * std::asin(std::sqrt(h));
}

// ---------------- Storage ----------------
// 伪代码：写入/覆盖节点
//   步骤1：若 uid 为空返回 InvalidArg；
//   步骤2：取独占锁，按 uid 覆盖写入 nodes_；
//   步骤3：维护名字索引（同 uid 同名不重复登记）。
Err Storage::put_node(const Node& n) {
  if (n.uid.empty()) return Err::InvalidArg;
  std::unique_lock lk(mu_);
  nodes_[n.uid] = n;
  std::string key = std::to_string((int)n.kind) + "|" + n.name;
  auto& v = name_index_[key];
  if (std::find(v.begin(), v.end(), n.uid) == v.end()) v.push_back(n.uid);
  return Err::Ok;
}

// 伪代码：写边入口，等价于 upsert_edge(e, false)（同键边覆盖权重）
Err Storage::put_edge(const Edge& e) { return upsert_edge(e, false); }

// 伪代码：upsert 边（出/入两张邻接表同步维护）
//   步骤1：from/to 任一为空返回 InvalidArg；
//   步骤2：取独占锁；若两端节点任一不存在返回 NotFound；
//   步骤3：对目标边表执行 upd：遍历查找 (from,to,type) 相同的边——
//          找到则按 add_weight 决定权重累加或覆盖，并合入 attrs 后返回；
//          未找到则整体 push_back 新边；
//   步骤4：对 out_[from] 与 in_[to] 各执行一次 upd，保证双表一致。
Err Storage::upsert_edge(const Edge& e, bool add_weight) {
  if (e.from.empty() || e.to.empty()) return Err::InvalidArg;
  std::unique_lock lk(mu_);
  if (!nodes_.count(e.from) || !nodes_.count(e.to)) return Err::NotFound;
  auto upd = [&](std::vector<Edge>& v) {
    for (auto& x : v) {
      if (x.from == e.from && x.to == e.to && x.type == e.type) {
        x.weight = add_weight ? x.weight + e.weight : e.weight;
        for (auto& kv : e.attrs) x.attrs[kv.first] = kv.second;
        return;
      }
    }
    v.push_back(e);
  };
  upd(out_[e.from]);
  upd(in_[e.to]);
  return Err::Ok;
}

// 伪代码：按 (from,to,type) 精确查边
//   步骤1：取共享锁，查 from 的出边表，不存在返回 nullptr；
//   步骤2：线性扫描出边，命中三元组相同者返回其地址，否则 nullptr。
const Edge* Storage::find_edge(const std::string& from, const std::string& to, RelType t) const {
  std::shared_lock lk(mu_);
  auto it = out_.find(from);
  if (it == out_.end()) return nullptr;
  for (auto& e : it->second)
    if (e.from == from && e.to == to && e.type == t) return &e;
  return nullptr;
}

// 按 uid 查节点（共享锁），不存在返回 nullptr；返回指针仅在持锁期间外谨慎使用
const Node* Storage::get_node(const std::string& uid) const {
  std::shared_lock lk(mu_);
  auto it = nodes_.find(uid);
  return it == nodes_.end() ? nullptr : &it->second;
}

// 按 uid 查节点（独占锁，可写版本），不存在返回 nullptr
Node* Storage::get_node_mut(const std::string& uid) {
  std::unique_lock lk(mu_);
  auto it = nodes_.find(uid);
  return it == nodes_.end() ? nullptr : &it->second;
}

// 伪代码：取节点邻居（出边 + 反向视角入边）
//   步骤1：取共享锁，构造结果集；
//   步骤2：定义 want：rel_types 为空则接受全部类型，否则仅接受列表内类型；
//   步骤3：定义 push：类型不符跳过；对端节点不存在跳过；否则把
//          (边, 对端节点指针) 追加到结果；
//   步骤4：遍历 uid 的出边表，对每条边 push(e, e.to)；
//   步骤5：遍历 uid 的入边表，将每条边复制后交换 from/to（翻转为 uid->邻居
//          视角）并在 attrs 标记 _reversed=1，再 push(r, e.from)。
std::vector<std::pair<Edge, const Node*>> Storage::neighbors(
    const std::string& uid, const std::vector<RelType>* rel_types) const {
  std::shared_lock lk(mu_);
  std::vector<std::pair<Edge, const Node*>> res;
  auto want = [&](RelType t) {
    if (!rel_types) return true;
    return std::find(rel_types->begin(), rel_types->end(), t) != rel_types->end();
  };
  auto push = [&](const Edge& e, const std::string& other) {
    if (!want(e.type)) return;
    auto it = nodes_.find(other);
    if (it == nodes_.end()) return;
    res.emplace_back(e, &it->second);
  };
  auto oi = out_.find(uid);
  if (oi != out_.end())
    for (auto& e : oi->second) push(e, e.to);
  auto ii = in_.find(uid);
  if (ii != in_.end()) {
    for (auto& e : ii->second) {
      // 入边以反向视角返回：调用方看到的边方向为 other -> uid，
      // 这里翻转为 uid -> other 视角，weight/type 不变，attrs 标记 reversed
      Edge r = e;
      std::swap(r.from, r.to);
      r.attrs["_reversed"] = "1";
      push(r, e.from);
    }
  }
  return res;
}

// 伪代码：余弦相似度
//   步骤1：任一向量为空或维数不等返回 0；
//   步骤2：单趟累加点积 dot 与两向量平方模 na、nb；
//   步骤3：任一模长 <= 0 返回 0，否则返回 dot/(sqrt(na)*sqrt(nb))。
double Storage::cosine(const Vec& a, const Vec& b) {
  if (a.empty() || b.empty() || a.size() != b.size()) return 0;
  double dot = 0, na = 0, nb = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += (double)a[i] * b[i];
    na += (double)a[i] * a[i];
    nb += (double)b[i] * b[i];
  }
  if (na <= 0 || nb <= 0) return 0;
  return dot / (std::sqrt(na) * std::sqrt(nb));
}

// 伪代码：暴力余弦 ANN topK
//   步骤1：取共享锁，全表扫描节点；embedding 为空的节点跳过；
//   步骤2：计算与查询向量的余弦相似度，仅保留相似度 > 0 的 (uid, s)；
//   步骤3：若候选数 > topk 用 partial_sort 取前 topk，否则全量 sort
//         （均按相似度降序）；
//   步骤4：截断到 topk 后返回。
std::vector<std::pair<std::string, double>> Storage::ann_search(const Vec& embedding, int topk) const {
  std::shared_lock lk(mu_);
  std::vector<std::pair<std::string, double>> all;
  all.reserve(nodes_.size());
  for (auto& kv : nodes_) {
    if (kv.second.embedding.empty()) continue;
    double s = cosine(embedding, kv.second.embedding);
    if (s > 0) all.emplace_back(kv.first, s);
  }
  if ((int)all.size() > topk)
    std::partial_sort(all.begin(), all.begin() + topk, all.end(),
                      [](auto& a, auto& b) { return a.second > b.second; });
  else
    std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.second > b.second; });
  if ((int)all.size() > topk) all.resize(topk);
  return all;
}

// 伪代码：半径内 Place 节点查询
//   步骤1：取共享锁，全表扫描；非 Place 节点跳过；
//   步骤2：haversine 距离 <= radius_m 的节点收入结果；
//   步骤3：按到查询点的距离升序排序后返回。
std::vector<const Node*> Storage::geo_lookup(double lat, double lng, double radius_m) const {
  std::shared_lock lk(mu_);
  std::vector<const Node*> res;
  for (auto& kv : nodes_) {
    const Node& n = kv.second;
    if (n.kind != NodeKind::Place) continue;
    if (haversine_m(lat, lng, n.lat, n.lng) <= radius_m) res.push_back(&n);
  }
  std::sort(res.begin(), res.end(), [&](const Node* a, const Node* b) {
    return haversine_m(lat, lng, a->lat, a->lng) < haversine_m(lat, lng, b->lat, b->lng);
  });
  return res;
}

// 伪代码：按 kind+name 精确查找——走名字索引 O(1)，取同名列表首个，否则 nullptr
const Node* Storage::find_by_name(NodeKind kind, const std::string& name) const {
  std::shared_lock lk(mu_);
  auto it = name_index_.find(std::to_string((int)kind) + "|" + name);
  if (it == name_index_.end() || it->second.empty()) return nullptr;
  auto n = nodes_.find(it->second.front());
  return n == nodes_.end() ? nullptr : &n->second;
}

// 伪代码：按 kind+name 枚举全部同名节点——名字索引取出 uid 列表逐个解析
std::vector<const Node*> Storage::find_all_by_name(NodeKind kind,
                                                   const std::string& name) const {
  std::shared_lock lk(mu_);
  std::vector<const Node*> res;
  auto it = name_index_.find(std::to_string((int)kind) + "|" + name);
  if (it == name_index_.end()) return res;
  for (auto& uid : it->second) {
    auto n = nodes_.find(uid);
    if (n != nodes_.end()) res.push_back(&n->second);
  }
  return res;
}

// 伪代码：按 attr 键值反查（强标识符）
//   步骤1：共享锁下全表扫描，kind 不符跳过；
//   步骤2：节点 attrs 中含 key 且值等于 val 则返回该节点；扫完未命中返回 nullptr。
const Node* Storage::find_by_attr(NodeKind kind, const std::string& key, const std::string& val) const {
  std::shared_lock lk(mu_);
  for (auto& kv : nodes_) {
    const Node& n = kv.second;
    if (n.kind != kind) continue;
    auto it = n.attrs.find(key);
    if (it != n.attrs.end() && it->second == val) return &n;
  }
  return nullptr;
}

// 伪代码：收集指定 kind 的全部节点：共享锁下全表扫描，kind 匹配者入结果
std::vector<const Node*> Storage::nodes_of_kind(NodeKind k) const {
  std::shared_lock lk(mu_);
  std::vector<const Node*> res;
  for (auto& kv : nodes_)
    if (kv.second.kind == k) res.push_back(&kv.second);
  return res;
}

// 统计边总数：共享锁下累加各节点出边表长度（入边表为同一份数据的镜像，不计）
size_t Storage::edge_count() const {
  std::shared_lock lk(mu_);
  size_t n = 0;
  for (auto& kv : out_) n += kv.second.size();
  return n;
}

// ---------------- 快照 ----------------
// 伪代码：节点 -> JSON
//   步骤1：填充全部标量字段（kind/precision 以数值存储枚举）；
//   步骤2：embedding 非空则转 JSON 数组写入；
//   步骤3：attrs 非空则转 JSON 对象写入。
static Json node_to_json(const Node& n) {
  Json j = Json::object();
  j["uid"] = Json::string(n.uid);
  j["kind"] = Json::number((double)n.kind);
  j["name"] = Json::string(n.name);
  j["text"] = Json::string(n.text);
  j["type"] = Json::string(n.type);
  j["ts"] = Json::number((double)n.ts);
  j["precision"] = Json::number((double)n.precision);
  j["importance"] = Json::number(n.importance);
  j["lat"] = Json::number(n.lat);
  j["lng"] = Json::number(n.lng);
  j["geohash"] = Json::string(n.geohash);
  j["status"] = Json::number(n.status);
  j["mention_count"] = Json::number(n.mention_count);
  if (!n.embedding.empty()) {
    Json e = Json::array();
    for (float f : n.embedding) e.push(Json::number(f));
    j["embedding"] = std::move(e);
  }
  if (!n.attrs.empty()) {
    Json a = Json::object();
    for (auto& kv : n.attrs) a[kv.first] = Json::string(kv.second);
    j["attrs"] = std::move(a);
  }
  return j;
}

// 伪代码：JSON -> 节点
//   步骤1：逐字段读取，缺失则用默认值（importance 默认 0.5，precision 默认 3，
//          其余为 0/空串）；枚举字段由数值强转；
//   步骤2：embedding/attrs 字段存在则逐项回填到向量/映射。
static Node node_from_json(const Json& j) {
  Node n;
  n.uid = j.find("uid") ? j.find("uid")->as_str() : "";
  n.kind = (NodeKind)(int)(j.find("kind") ? j.find("kind")->as_num() : 0);
  n.name = j.find("name") ? j.find("name")->as_str() : "";
  n.text = j.find("text") ? j.find("text")->as_str() : "";
  n.type = j.find("type") ? j.find("type")->as_str() : "";
  n.ts = (TimePoint)(j.find("ts") ? j.find("ts")->as_num() : 0);
  n.precision = (Precision)(int)(j.find("precision") ? j.find("precision")->as_num() : 3);
  n.importance = j.find("importance") ? j.find("importance")->as_num(0.5) : 0.5;
  n.lat = j.find("lat") ? j.find("lat")->as_num() : 0;
  n.lng = j.find("lng") ? j.find("lng")->as_num() : 0;
  n.geohash = j.find("geohash") ? j.find("geohash")->as_str() : "";
  n.status = (int)(j.find("status") ? j.find("status")->as_num() : 0);
  n.mention_count = (int)(j.find("mention_count") ? j.find("mention_count")->as_num() : 0);
  if (auto* e = j.find("embedding"))
    for (auto& v : e->arr) n.embedding.push_back((float)v.as_num());
  if (auto* a = j.find("attrs"))
    for (auto& kv : a->obj) n.attrs[kv.first] = kv.second.as_str();
  return n;
}

// 伪代码：边 -> JSON：写 from/to/weight，type 用 rel_name 转为可读名字符串；
//          attrs 非空则附加为 JSON 对象
static Json edge_to_json(const Edge& e) {
  Json j = Json::object();
  j["from"] = Json::string(e.from);
  j["to"] = Json::string(e.to);
  j["type"] = Json::string(rel_name(e.type));
  j["weight"] = Json::number(e.weight);
  if (!e.attrs.empty()) {
    Json a = Json::object();
    for (auto& kv : e.attrs) a[kv.first] = Json::string(kv.second);
    j["attrs"] = std::move(a);
  }
  return j;
}

// 伪代码：序列化为 JSON 对象（内存版，save 的纯数据部分）
//   步骤1：取共享锁，全部节点 → nodes 数组、全部出边（入边为镜像不重复导出）→ edges 数组。
Json Storage::to_json() const {
  std::shared_lock lk(mu_);
  Json root = Json::object();
  Json jn = Json::array();
  for (auto& kv : nodes_) jn.push(node_to_json(kv.second));
  Json je = Json::array();
  for (auto& kv : out_)
    for (auto& e : kv.second) je.push(edge_to_json(e));
  root["nodes"] = std::move(jn);
  root["edges"] = std::move(je);
  return root;
}

// 伪代码：从 JSON 对象重建（load 的纯数据部分）
//   步骤1：取独占锁，清空 nodes_/out_/in_/名字索引；
//   步骤2：逐节点反序列化并同步重建名字索引；
//   步骤3：逐边还原（type 由名字反查，无法识别跳过），重建双邻接表。
Err Storage::from_json(const Json& root) {
  std::unique_lock lk(mu_);
  nodes_.clear();
  out_.clear();
  in_.clear();
  name_index_.clear();
  if (auto* jn = root.find("nodes"))
    for (auto& j : jn->arr) {
      Node n = node_from_json(j);
      // 同步重建名字索引（与 put_node 同一语义）
      name_index_[std::to_string((int)n.kind) + "|" + n.name].push_back(n.uid);
      nodes_[n.uid] = std::move(n);
    }
  if (auto* je = root.find("edges"))
    for (auto& j : je->arr) {
      Edge e;
      e.from = j.find("from") ? j.find("from")->as_str() : "";
      e.to = j.find("to") ? j.find("to")->as_str() : "";
      auto t = rel_from_name(j.find("type") ? j.find("type")->as_str() : "");
      if (!t) continue;
      e.type = *t;
      e.weight = j.find("weight") ? j.find("weight")->as_num(1.0) : 1.0;
      if (auto* a = j.find("attrs"))
        for (auto& kv : a->obj) e.attrs[kv.first] = kv.second.as_str();
      out_[e.from].push_back(e);
      in_[e.to].push_back(e);
    }
  return Err::Ok;
}

// 伪代码：保存快照
//   步骤1：to_json 取数据；步骤2：trunc 写文件，失败 IoError。
Err Storage::save(const std::string& path) const {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return Err::IoError;
  f << to_json().dump();
  return f.good() ? Err::Ok : Err::IoError;
}

// 伪代码：载入快照
//   步骤1：读入整个文件，打不开返回 IoError；JSON 解析失败返回 ParseError；
//   步骤2：交 from_json 重建。
Err Storage::load(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return Err::IoError;
  std::stringstream ss;
  ss << f.rdbuf();
  bool ok = false;
  Json root = Json::parse(ss.str(), &ok);
  if (!ok) return Err::ParseError;
  return from_json(root);
}

}  // namespace ame
