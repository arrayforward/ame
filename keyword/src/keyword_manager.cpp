/*
 * 模块：M3 关键词管理器（keyword）
 * 定位：关键词归一化、停用词过滤、Keyword 节点生命周期管理与 RELATED 关联边维护的实现文件。
 *
 * 本文件主体思路：
 *   实现 KeywordManager 的全部接口。核心流程为：别名表归一 -> 停用词过滤 ->
 *   在存储层查找/创建 Keyword 节点；已存在节点累加提及计数并按阈值唤醒，
 *   新节点默认休眠；RELATED 边按关系类型决定是否写双向边。
 *
 * 关键算法/数据结构：
 *   - 别名归一与停用词判定均为哈希表 O(1) 查找；
 *   - 休眠/唤醒为阈值状态机：新词默认休眠（status=1），mention_count>=3 且
 *     有边连接时唤醒（status=0）；
 *   - 双向边处理依据 core::rel_undirected 对无向关系补写反向边；
 *   - 无复杂算法。
 *
 * 依赖关系：
 *   - 依赖 core 公共层：ame/core/uid.h（gen_uid 生成节点 uid）、
 *     ame/core/types.h（经头文件引入：Node/Edge/RelType/rel_undirected）；
 *   - 依赖 M9 存储引擎 Storage：find_by_name / get_node / get_node_mut /
 *     put_node / upsert_edge / neighbors / nodes_of_kind；
 *   - 不依赖其它功能模块。
 */
#include "ame/keyword/keyword_manager.h"

#include "ame/core/json.h"
#include "ame/core/uid.h"
#include "ame/storage/storage.h"

#include <cctype>

namespace ame {

// 受控根主题表：返回静态内置常量列表（约 30 个主题），进程内只初始化一次。
const std::vector<std::string>& KeywordManager::root_topics() {
  static const std::vector<std::string> kTopics = {
      "工作", "健康", "理财", "家庭", "学习", "出行", "饮食", "运动", "社交", "娱乐",
      "居住", "车辆", "数码", "旅行", "情感", "睡眠", "医疗", "教育", "购物", "宠物",
      "法律", "装修", "育儿", "养老", "保险", "投资", "餐饮", "穿搭", "游戏", "音乐"};
  return kTopics;
}

// 构造函数伪代码：
//   步骤1：保存存储引擎引用；
//   步骤2：初始化内置别名表（如 睡不着/失眠症 -> 失眠，后续可经 add_alias 动态扩展）；
//   步骤3：初始化停用词黑名单（的/了/在 等无信息量虚词）。
KeywordManager::KeywordManager(Storage& storage) : storage_(storage) {
  // 内置别名表（可动态扩展）
  alias_ = {{"睡不着", "失眠"}, {"失眠症", "失眠"}, {"睡不好", "失眠"},
            {"压力大", "压力"}, {"压力过大", "压力"}, {"心烦", "焦虑"},
            {"感冒了", "感冒"}, {"车子", "车"},   {"单车", "自行车"}};
  for (auto& [a, s] : alias_) builtin_standards_.insert(s);  // 内置标准词保护集
  // 停用词黑名单
  stopwords_ = {"的", "了", "在", "我", "我们", "你", "他", "她", "和", "就",
                "都", "而", "及", "与", "着", "或", "一个", "没有", "是", "很"};
}

// 归一化：ASCII 统一小写（查询侧与写入侧词形对齐的通用规则，中文不受影响），
//   再查别名表，命中返回标准词，未命中返回小写化结果。
std::string KeywordManager::normalize(const std::string& word) const {
  std::string low;
  low.reserve(word.size());
  for (unsigned char c : word) low += (char)(c < 128 ? std::tolower(c) : c);
  auto it = alias_.find(low);
  return it != alias_.end() ? it->second : low;
}

// 动态注册一条别名映射（alias -> standard）。
// 保护内置标准词：alias 若是内置别名表的标准词（如 失眠），拒绝被重映射
// （防大词典链式改写，如 失眠→辗转反侧）；词典内部重映射自由。
void KeywordManager::add_alias(const std::string& alias, const std::string& standard) {
  if (alias == standard) return;
  if (builtin_standards_.count(alias)) return;  // 内置标准词不可改写
  alias_[alias] = standard;
}

// 别名表导出：{"aliases": {alias: standard, ...}} 对象段（含内置表，导入时覆盖写幂等）。
Json KeywordManager::aliases_to_json() const {
  Json j = Json::object();
  for (auto& [a, s] : alias_) j[a] = Json::string(s);
  return j;
}

// 别名表导入：逐条 add_alias（覆盖写，幂等）。
void KeywordManager::aliases_from_json(const Json& j) {
  if (j.kind != Json::Obj) return;
  for (auto& [a, s] : j.obj) add_alias(a, s.as_str());
}

// variants 伪代码：线性扫别名表，收集 normalize 后等于 standard 的别名（词典同义扩展用），
//   最多 cap 个（防爆）。
std::vector<std::string> KeywordManager::variants(const std::string& standard, int cap) const {
  std::vector<std::string> out;
  for (auto& [alias, std_word] : alias_) {
    if (std_word == standard && alias != standard) {
      out.push_back(alias);
      if ((int)out.size() >= cap) break;
    }
  }
  return out;
}

// add_near 伪代码：登记一条近义词（词典灌库用，截断至 8 条防爆）。
void KeywordManager::add_near(const std::string& word, const std::string& near, double score) {
  auto& v = near_[word];
  if ((int)v.size() < 8) v.emplace_back(near, score);
}

// near_words 伪代码：取 word 的近义词前 cap 个（score 已在灌库时按 >0.6 过滤）。
std::vector<std::pair<std::string, double>> KeywordManager::near_words(const std::string& word,
                                                                       int cap) const {
  auto it = near_.find(word);
  if (it == near_.end()) return {};
  std::vector<std::pair<std::string, double>> out(it->second.begin(), it->second.end());
  if ((int)out.size() > cap) out.resize(cap);
  return out;
}

// 停用词判定：哈希集合查找。
bool KeywordManager::is_stopword(const std::string& word) const {
  return stopwords_.count(word) > 0;
}

// 按标准词名在存储层查找 Keyword 节点，未找到返回 nullptr。
const Node* KeywordManager::find(const std::string& standard_word) const {
  return storage_.find_by_name(NodeKind::Keyword, standard_word);
}

// get_or_create 伪代码：
//   步骤1：created 输出参数先置 false；
//   步骤2：对输入词做别名归一；
//   步骤3：若归一结果为空串或命中停用词，直接返回空串（不落库）；
//   步骤4：按标准词查找已有 Keyword 节点：
//     - 若存在：提及计数 +1；若节点处于休眠（status==1）且计数已达阈值 3
//       且已有边连接，则唤醒（status=0）；返回已有节点 uid；
//     - 若不存在：构造新节点（gen_uid 生成 uid、name=标准词、type=类别、
//       计数=1、status=1 默认休眠），写入存储层，created 置 true，返回新 uid。
std::string KeywordManager::get_or_create(const std::string& word, const std::string& category,
                                          bool* created) {
  if (created) *created = false;
  std::string std_word = normalize(word);
  if (is_stopword(std_word) || std_word.empty()) return "";

  const Node* exist = find(std_word);
  if (exist) {
    Node* m = storage_.get_node_mut(exist->uid);
    m->mention_count++;
    // 唤醒：计数达到阈值且有边连接
    if (m->status == 1 && m->mention_count >= 3 &&
        !storage_.neighbors(m->uid).empty()) {
      m->status = 0;
    }
    return exist->uid;
  }

  Node n;
  n.uid = gen_uid();
  n.kind = NodeKind::Keyword;
  n.name = std_word;
  n.type = category;
  n.mention_count = 1;
  n.status = 1;  // 新词默认休眠（出现<3 且无边），后续边连接/计数增长时唤醒
  storage_.put_node(n);
  if (created) *created = true;
  return n.uid;
}

// link_related 伪代码：
//   步骤1：对 a、b 两个词分别归一后查找对应 Keyword 节点；
//   步骤2：任一端不存在则返回 Err::NotFound；
//   步骤3：构造 a->b 的边并 upsert 写入存储层；
//   步骤4：若该关系类型为无向（rel_undirected，如 NEAR/共现/相似/共同经历），
//          再补写一条 b->a 的反向边；
//   步骤5：返回正向边的写入结果。
Err KeywordManager::link_related(const std::string& a, const std::string& b, RelType type,
                                 double weight) {
  const Node* na = find(normalize(a));
  const Node* nb = find(normalize(b));
  if (!na || !nb) return Err::NotFound;
  Edge e{na->uid, nb->uid, type, weight, {}};
  Err r = storage_.upsert_edge(e, false);
  if (rel_undirected(type)) {
    Edge rev{nb->uid, na->uid, type, weight, {}};
    storage_.upsert_edge(rev, false);
  }
  return r;
}

// list_keywords 伪代码：
//   步骤1：遍历存储层全部 Keyword 节点；
//   步骤2：若指定了 category 且节点类别不匹配则跳过；
//   步骤3：若 status >= 0（即启用了状态过滤）且节点状态不匹配则跳过；
//   步骤4：其余节点加入结果集返回。
std::vector<const Node*> KeywordManager::list_keywords(const std::string& category,
                                                       int status) const {
  std::vector<const Node*> res;
  for (auto* n : storage_.nodes_of_kind(NodeKind::Keyword)) {
    if (!category.empty() && n->type != category) continue;
    if (status >= 0 && n->status != status) continue;
    res.push_back(n);
  }
  return res;
}

// 手动休眠：归一后找到节点则置 status=1，未找到则静默忽略。
void KeywordManager::sleep_keyword(const std::string& word) {
  const Node* n = find(normalize(word));
  if (n) storage_.get_node_mut(n->uid)->status = 1;
}

// 手动唤醒：归一后找到节点则置 status=0，未找到则静默忽略。
void KeywordManager::wake_keyword(const std::string& word) {
  const Node* n = find(normalize(word));
  if (n) storage_.get_node_mut(n->uid)->status = 0;
}

// 休眠判定：按 uid 取节点，仅当节点存在、为 Keyword 类型且 status==1 时返回 true。
bool KeywordManager::is_dormant(const std::string& uid) const {
  const Node* n = storage_.get_node(uid);
  return n && n->kind == NodeKind::Keyword && n->status == 1;
}

}  // namespace ame
