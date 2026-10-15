/*
 * =============================================================================
 * 模块定位：M1 要素提取器 —— RuleExtractor 的规则实现，自由文本 → 结构化记忆要素。
 *
 * 本文件主体思路：
 *   在不引入 LLM 的前提下，用纯启发式规则从一段中文文本中提取五类要素：
 *   地点（地名词典最长优先匹配）、实体（"老X/小X"称呼模式 + 物品词典）、
 *   关键词（分类词典匹配并去重）、事件类型（取第一个 action 类关键词）、
 *   事件时间（相对时间词换算为时间戳）。词典可通过 add_place/add_keyword
 *   在运行时扩展。
 *
 * 关键算法/数据结构：
 *   无复杂算法。核心是子串匹配（std::string::find）配合"按词条长度降序排序
 *   实现最长优先匹配"；中文按 UTF-8 编码处理，每个汉字占 3 字节，称呼模式
 *   截取前缀后 3 字节作为一个汉字人名。词典用 vector 顺序表存储。
 *
 * 依赖关系：
 *   依赖本模块头文件 ame/extractor/extractor.h（接口与数据结构）、
 *   M10 时间服务 ame/time/time_service.h（TimeService::now_utc 取当前 UTC
 *   时间作为相对时间词换算的基准）。
 * =============================================================================
 */
#include "ame/extractor/extractor.h"

#include "ame/time/time_service.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_set>

namespace ame {

// 构造函数：初始化内置地名词典与关键词词典。
// 伪代码：
//   步骤1：写入内置地名词典（北京一带样例坐标，词条 = 名称 + 经纬度）。
//   步骤2：写入内置关键词词典 word -> category
//          （category 取值 action|attr|topic|sentiment|concept）。
RuleExtractor::RuleExtractor() {
  // 内置地名词典（北京一带样例坐标）
  places_ = {
      {"朝阳公园", 39.9449, 116.4781}, {"三里屯", 39.9355, 116.4542},
      {"望京", 39.9965, 116.4730},     {"中关村", 39.9833, 116.3106},
      {"咖啡馆", 39.9455, 116.4790},   {"公司", 39.9087, 116.3975},
      {"健身房", 39.9440, 116.4770},   {"医院", 39.9370, 116.4600},
  };
  // 内置关键词词典 word -> category（action|attr|topic|sentiment|concept）
  kw_dict_ = {
      {"爆胎", "action"},   {"维修", "action"}, {"保养", "action"},
      {"聚餐", "action"},   {"谈项目", "action"}, {"加班", "action"},
      {"开会", "action"},   {"跑步", "action"}, {"体检", "action"},
      {"红色", "attr"},     {"二手", "attr"},   {"坏了", "attr"},
      {"工作", "topic"},    {"健康", "topic"},  {"理财", "topic"},
      {"开心", "sentiment"}, {"踩坑", "sentiment"}, {"值得", "sentiment"},
      {"失眠", "concept"},  {"睡不着", "concept"}, {"压力", "concept"}, {"焦虑", "concept"},
      {"拖延", "concept"},  {"性价比", "concept"}, {"咖啡", "concept"},
      {"感冒", "concept"},
  };
}

// 向地名词典追加一条词条（名称 + 经纬度），供 demo/测试扩展。
void RuleExtractor::add_place(const std::string& name, double lat, double lng) {
  places_.push_back({name, lat, lng});
}

// 向关键词词典追加一条词条（词 + 类别），供 demo/测试扩展。
void RuleExtractor::add_keyword(const std::string& word, const std::string& category) {
  kw_dict_.emplace_back(word, category);
}

// 子串匹配辅助：判断 word 是否出现在 text 中，若出现且 pos 非空则回写首次出现位置。
// 按文本中出现位置取最长匹配优先
static bool contains_at(const std::string& text, const std::string& word, size_t* pos) {
  size_t p = text.find(word);
  if (p == std::string::npos) return false;
  if (pos) *pos = p;
  return true;
}

// 核心提取入口：对一段自由文本执行完整的规则提取流程，返回结构化 Extraction。
// 伪代码：
//   步骤1：初始化结果 ex，事件原文记为输入文本，其余字段取默认值。
//   步骤2【地点】：将地名词典按名称长度降序排序（最长优先），逐个做子串匹配；
//          命中第一个即写入地点（名称 + 经纬度 + has_coord=true）并停止，至多取一个地点。
//   步骤3【实体·人物】：扫描称呼前缀"老"/"小"：每处命中截取其后 3 字节
//          （UTF-8 一个汉字）拼成人名，去重后作为 role=人物、type=person 的实体加入。
//   步骤4【实体·客体】：遍历内置物品词表（自行车/汽车/车/手机/电脑），
//          命中且未重复者作为 role=客体、type=object 的实体加入。
//   步骤5【关键词】：复制关键词词典并按词长降序排序；逐词匹配文本，
//          若该词已被某个实体 surface 包含（视为被实体覆盖）则跳过，否则连同类别记入。
//   步骤6【事件类型】：取关键词列表中第一个 category==action 的词作为事件类型；
//          若无 action 词则保持默认 "generic"。
//   步骤7【事件时间】：以当前 UTC 时间为基准，按相对时间词换算（按优先级互斥分支）：
//          若含"前天"则 now-2 天、精度 Day；否则若含"昨天"则 now-1 天、Day；
//          否则若含"今天"则 now、Day；否则若含"上个月"则 now-30 天、Month；
//          否则若含"去年"则 now-365 天、Year；均不含则默认 now、Day。
//   步骤8：返回 ex。
Extraction RuleExtractor::extract(const std::string& text) {
  Extraction ex;
  ex.event.text = text;

  // ---- 地点：最长地名优先 ----
  std::sort(places_.begin(), places_.end(),
            [](auto& a, auto& b) { return a.name.size() > b.name.size(); });
  for (auto& p : places_) {
    if (contains_at(text, p.name, nullptr)) {
      ex.place = {p.name, p.lat, p.lng, true};
      break;
    }
  }

  // ---- 实体：称呼模式 "老X"/"小X"/"X哥/X姐" + 组织/物品词典 ----
  // UTF-8 中文每字 3 字节；扫描称呼前缀
  auto scan_honorific = [&](const char* prefix, size_t plen) {
    size_t pos = 0;
    while ((pos = text.find(prefix, pos)) != std::string::npos) {
      size_t start = pos + plen;
      if (start + 3 <= text.size()) {
        std::string name = std::string(prefix) + text.substr(start, 3);  // 老+一个汉字
        bool dup = false;
        for (auto& e : ex.entities)
          if (e.surface == name) { dup = true; break; }
        if (!dup) ex.entities.push_back({name, "人物", "person", {}});
      }
      pos = start;
    }
  };
  scan_honorific("老", 3);
  scan_honorific("小", 3);
  // 常见物品/组织词
  static const char* kObjects[] = {"自行车", "汽车", "车", "手机", "电脑"};
  for (auto* w : kObjects) {
    if (contains_at(text, w, nullptr)) {
      bool dup = false;
      for (auto& e : ex.entities) if (e.surface == w) { dup = true; break; }
      if (!dup) ex.entities.push_back({w, "客体", "object", {}});
    }
  }

  // ---- 关键词：词典匹配（长词优先，去重，跳过已被实体/地点覆盖的词）----
  std::vector<std::pair<std::string, std::string>> dict = kw_dict_;
  std::sort(dict.begin(), dict.end(),
            [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
  for (auto& [w, cat] : dict) {
    if (!contains_at(text, w, nullptr)) continue;
    bool covered = false;
    for (auto& e : ex.entities)
      if (e.surface.find(w) != std::string::npos) { covered = true; break; }
    if (covered) continue;
    ex.keywords.push_back({w, cat});
  }

  // ---- 关键词：AC 自动机一遍扫描（load_keyword_dict 加载的大词表）----
  // 大小写不敏感：对 ASCII 小写副本扫描（词表存小写；中文码点不受 tolower 影响）。
  // 输出词条为词表词（小写），M3 normalize 同为小写语义，写入/查询两侧一致。
  if (ac_built_) {
    std::string scan_text = text;
    for (auto& ch : scan_text)
      if ((unsigned char)ch < 128 && ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    for (auto& [pos, w, cat] : ac_match(scan_text)) {
      (void)pos;
      bool covered = false;
      for (auto& e : ex.entities)
        if (e.surface.find(w) != std::string::npos) { covered = true; break; }
      if (covered) continue;
      bool dup = false;
      for (auto& k : ex.keywords)
        if (k.word == w) { dup = true; break; }
      if (!dup) ex.keywords.push_back({w, cat});
    }
  }

  // ---- 英文自由词元（通用锚点，供英文语料提取；与中文词典互补）----
  // 伪代码：
  //   步骤1：线性扫描做 ASCII 词切分（isalnum 连续段），跟踪句首标志
  //          （'.'/'!'/'?' 后置位，词后清零）；
  //   步骤2：非句首的首字母大写词（长度≥2）→ person 实体（专名启发式），去重；
  //          连续大写词（仅空格分隔）合并为复合专名（"New York" 不拆成两个实体）；
  //   步骤3：其余长度≥4 且不在英文停用词表的词 → 小写化后作 attr 类关键词，
  //          已被提取为实体的词跳过，去重。
  {
    static const std::unordered_set<std::string> kEnStop = {
        "this", "that", "with", "have", "from", "they", "them", "what", "when",
        "where", "which", "there", "their", "would", "could", "should", "about",
        "just", "like", "know", "really", "yeah", "okay", "well", "then", "than",
        "some", "into", "over", "such", "only", "also", "very", "much", "been"};
    bool sent_start = true;
    size_t prev_end = 0;     // 上一个词元结束位置
    bool prev_cap = false;   // 上一个词元是否大写开头
    size_t i = 0;
    while (i < text.size()) {
      unsigned char c = (unsigned char)text[i];
      if (c < 128 && std::isalpha(c)) {
        size_t j = i;
        while (j < text.size() && (unsigned char)text[j] < 128 &&
               std::isalnum((unsigned char)text[j]))
          ++j;
        std::string tok = text.substr(i, j - i);
        std::string low;
        for (char ch : tok) low += (char)std::tolower((unsigned char)ch);
        bool cap = std::isupper((unsigned char)tok[0]) != 0;
        // 复合专名合并：上一个词元大写、本词元大写、中间仅空格 → 并入上一个实体
        bool merged = false;
        if (cap && prev_cap && i > prev_end && i - prev_end <= 2 &&
            !ex.entities.empty()) {
          std::string& last = ex.entities.back().surface;
          std::string joiner = text.substr(prev_end, i - prev_end);
          if (joiner.find_first_not_of(' ') == std::string::npos) {
            last += joiner + tok;
            merged = true;
          }
        }
        if (merged) {
          // 已并入复合实体
        } else if (cap && tok.size() >= 2 && !sent_start && low != "i") {
          bool dup = false;
          for (auto& e : ex.entities)
            if (e.surface == tok) { dup = true; break; }
          if (!dup) ex.entities.push_back({tok, "人物", "person", {}});
        } else if (tok.size() >= 4 && !kEnStop.count(low)) {
          bool dup = false;
          for (auto& e : ex.entities) {
            std::string elow;
            for (char ch : e.surface) elow += (char)std::tolower((unsigned char)ch);
            if (elow == low) { dup = true; break; }
          }
          for (auto& k : ex.keywords)
            if (k.word == low) { dup = true; break; }
          if (!dup) ex.keywords.push_back({low, "attr"});
        }
        prev_cap = cap;
        prev_end = j;
        sent_start = false;
        i = j;
      } else {
        if (c == '.' || c == '!' || c == '?') sent_start = true;
        ++i;
      }
    }
  }

  // ---- 事件类型：取第一个 action 关键词 ----
  for (auto& k : ex.keywords)
    if (k.category == "action") { ex.event.type = k.word; break; }

  // ---- 时间词：昨天/今天/前天/去年/上个月/深夜... ----
  TimePoint now = TimeService::now_utc();
  if (contains_at(text, "前天", nullptr)) {
    ex.event.time = now - 2 * 86400; ex.event.precision = Precision::Day;
  } else if (contains_at(text, "昨天", nullptr)) {
    ex.event.time = now - 86400; ex.event.precision = Precision::Day;
  } else if (contains_at(text, "今天", nullptr)) {
    ex.event.time = now; ex.event.precision = Precision::Day;
  } else if (contains_at(text, "上个月", nullptr)) {
    ex.event.time = now - 30LL * 86400; ex.event.precision = Precision::Month;
  } else if (contains_at(text, "去年", nullptr)) {
    ex.event.time = now - 365LL * 86400; ex.event.precision = Precision::Year;
  } else {
    ex.event.time = now; ex.event.precision = Precision::Day;
  }
  return ex;
}

}  // namespace ame
