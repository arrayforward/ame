/*
 * 所属模块：bench 基准适配层——姓名别名表灌库（3.0 刀2：apply_lexicon 同义/近义/
 *   反义灌库已裁——RELATED 边端到端贡献≈0（docs/reports 归档）；词库资产文件保留
 *   供报告复跑，无管线消费；names_zh 夹具 6/6 的 name-alias 为有效资产保留）。
 * 本文件主体思路：names_zh.json → M3 别名表 + M1 提取词面 + M2 实体/Alias
 *   （人名/专名归一域，严格等价；须在摄入前调用）。
 * 依赖关系：依赖 bench.h、M12 engine、M3 keyword；被 CLI 依赖。
 */
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "ame/bench/bench.h"
#include "ame/core/json.h"
#include "ame/engine/engine.h"

namespace ame {

// apply_lexicon 伪代码（同义/近义/反义三类分流，设计文档 M3 原义）：
//   步骤1：读取并解析 lexicon.json（entries 数组）；
//   步骤2：syn_strong（同义，含旧版 synonyms 字段兼容）→ M3 别名归一表
//         （synonym -> 词条主词）；
//   步骤3：syn_near（近义）→ 双方节点存在且 score>0.6、每词 ≤8 条 → REL_SIMILAR 边，
//         权重取 score（保留双节点，只经图扩散生效，不进别名表）；
//   步骤4：antonyms → 双方节点存在 → REL_OPPOSITE 边（core 系数 0.5，最低传导，
//         扩散中不会被当强联想）；
//   步骤5：返回灌入条目数（applied 出参）。
// 源区分（复活工程 R1 → 第六轮修订）：root 带 "source":"distilled"（或 distilled-sim）
//   的蒸馏产物不再写别名表（graded 边表替代等价归一，见 §14 退役清单）；
//   无 source 字段的源（wordnet/中文）维持 alias_ok 守卫，行为逐比特不变。
Err apply_lexicon(Engine& engine, const std::string& path, int* applied) {
  std::ifstream f(path);
  if (!f) return Err::IoError;
  std::stringstream ss;
  ss << f.rdbuf();
  bool ok = false;
  Json root = Json::parse(ss.str(), &ok);
  if (!ok) return Err::ParseError;
  const Json* entries = root.find("entries");
  if (!entries) return Err::ParseError;
  const Json* src = root.find("source");
  const bool distilled = src && src->as_str() == "distilled";
  int n = 0;
  // 主词集合与主词→strong 表（防链式/互指重映射：本身是主词的词，仅当与当前词
  // 互为同义组成员（双向 strong 互指）时才允许归并；多义词单向互指跳过）
  std::unordered_map<std::string, std::unordered_set<std::string>> strong_of;
  for (auto& e : entries->arr) {
    const Json* w = e.find("word");
    if (!w || w->as_str().empty()) continue;
    std::unordered_set<std::string> ss;
    const Json* strong = e.find("syn_strong");
    if (!strong) strong = e.find("synonyms");
    if (strong)
      for (auto& s : strong->arr) ss.insert(s.as_str());
    strong_of[w->as_str()] = std::move(ss);
  }
  auto alias_ok = [&](const std::string& word, const std::string& syn) {
    auto it = strong_of.find(syn);
    if (it == strong_of.end()) return true;          // 非主词：直接归并
    return it->second.count(word) > 0;               // 主词：仅互为同义组才归并
  };
  for (auto& e : entries->arr) {
    const Json* w = e.find("word");
    if (!w || w->as_str().empty()) continue;
    std::string word = w->as_str();
    // 同义 → 别名归并（syn_strong，兼容旧版 synonyms 字段）：
    // 蒸馏源（source=distilled / distilled-sim）第六轮起彻底旁路等价归一——词级语义
    // 走 WordEdgeTable 的 graded 边（--word-edges），不再写死别名改写；其余源维持
    // 双向守卫（防多义词互指稀释）。near/antonyms 两类边照灌（图扩散消费）。
    const Json* strong = e.find("syn_strong");
    if (!strong) strong = e.find("synonyms");
    if (strong && !distilled)
      for (auto& s : strong->arr)
        if (!s.as_str().empty() && alias_ok(word, s.as_str())) {
          engine.keywords().add_alias(s.as_str(), word);
          ++n;
        }
    // 近义 → REL_SIMILAR 边（保留双节点；score>0.6、≤8 条）；
    // 同时登记近义词表（供条件归一降级扩展，不要求节点存在）
    const Json* near = e.find("syn_near");
    if (near) {
      int cnt = 0;
      for (auto& r : near->arr) {
        if (cnt >= 8) break;
        const Json* rw = r.find("word");
        const Json* rs = r.find("score");
        double score = rs ? rs->as_num(0.7) : 0.7;
        if (!rw || score <= 0.6) continue;
        engine.keywords().add_near(word, rw->as_str(), score);
        ++cnt;
        if (engine.keywords().find(word) && engine.keywords().find(rw->as_str())) {
          engine.keywords().link_related(word, rw->as_str(), RelType::REL_SIMILAR, score);
          ++n;
        }
      }
    }
    // 反义 → REL_OPPOSITE 边（系数 0.5，扩散最低档）
    const Json* ant = e.find("antonyms");
    if (ant)
      for (auto& s : ant->arr) {
        if (engine.keywords().find(word) && engine.keywords().find(s.as_str())) {
          engine.keywords().link_related(word, s.as_str(), RelType::REL_OPPOSITE, 1.0);
          ++n;
        }
      }
    // 旧版 related 字段兼容（视作近义）
    const Json* rel = e.find("related");
    if (rel && !near)
      for (auto& r : rel->arr) {
        const Json* rw = r.find("word");
        const Json* rs = r.find("score");
        if (rw && rs && rs->as_num() > 0.6 && engine.keywords().find(word) &&
            engine.keywords().find(rw->as_str())) {
          engine.keywords().link_related(word, rw->as_str(), RelType::REL_SIMILAR,
                                         rs->as_num());
          ++n;
        }
      }
  }
  if (applied) *applied = n;
  return Err::Ok;
}

Err apply_name_aliases(Engine& engine, const std::string& path, int* applied) {
  std::ifstream f(path);
  if (!f) return Err::IoError;
  std::stringstream ss;
  ss << f.rdbuf();
  bool ok = false;
  Json root = Json::parse(ss.str(), &ok);
  if (!ok) return Err::ParseError;
  const Json* groups = root.find("groups");
  if (!groups || groups->kind != Json::Arr) return Err::ParseError;
  int n = 0;
  for (auto& g : groups->arr) {
    const Json* cj = g.find("canonical");
    if (!cj || cj->as_str().empty()) continue;
    std::string canonical = cj->as_str();
    std::vector<std::string> aliases;
    const Json* aj = g.find("aliases");
    if (aj && aj->kind == Json::Arr)
      for (auto& a : aj->arr)
        if (!a.as_str().empty() && a.as_str() != canonical) aliases.push_back(a.as_str());
    // M3：严等价别名（守卫在 add_alias 内部，原样生效）
    for (auto& a : aliases) engine.keywords().add_alias(a, canonical);
    // M1：词面注册（concept 类），摄入/查询两侧提取均可见
    engine.extractor().add_keyword(canonical, "concept");
    for (auto& a : aliases) engine.extractor().add_keyword(a, "concept");
    // M2：canonical 建档 + 全变体 Alias 节点（幂等：已存在则不重复建档）
    if (!engine.storage().find_by_name(NodeKind::Entity, canonical))
      engine.register_entity("person", canonical, {}, aliases);
    n += (int)aliases.size() + 1;
  }
  if (applied) *applied = n;
  return Err::Ok;
}

}  // namespace ame
