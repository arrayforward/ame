// ============================================================================
// 模块：M6 扩散引擎（diffusion）——联想记忆的"思考主体"，从种子节点出发沿
//       联想图做加权能量扩散，产出各节点的激活能量与可解释传播路径。
//
// 本文件主体思路：实现 DiffusionEngine。核心为 diffuse() 的逐层波前扩展：
// 种子入队为第 0 跳波前，每层把波前节点的出边邻居算入下一层，边传导系数
// 与 1/(hop+1) 衰减共同决定子节点能量，直至深度耗尽、budget 用尽或能量枯竭。
//
// 关键算法/数据结构：加权逐层 BFS（vector 作波前队列，unordered_set 判重）。
//   - 边传导 = conduction_coeff(类型) × weight × 可选类型乘子；
//     weight > 5 的高频边按 5 + log1p(w-5) 软化。
//   - 深度硬限 [2,3]；budget 限制已扩展节点总数；child_e < 0.01 能量剪枝。
//   - 每节点 paths 按能量降序保留 top3，排序截断维护。
//   - 同一节点在同一 hop 层内只入队一次（能量仍可多次累加）。
//
// 依赖关系：依赖 M9 storage（Storage::get_node / neighbors 邻接表查询）；
// 依赖 M3 keyword（KeywordManager，休眠关键词判定的预留入口，当前直接查
// 节点 status）；依赖 core 公共层（conduction_coeff、RelType、NodeKind）。
// ============================================================================
#include "ame/diffusion/diffusion_engine.h"

#include "ame/keyword/keyword_manager.h"
#include "ame/storage/storage.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_set>

namespace ame {

// 构造函数：保存存储引擎引用与可选的关键词管理器指针（kw 可空，空则查节点 status）。
DiffusionEngine::DiffusionEngine(Storage& storage, KeywordManager* kw)
    : storage_(storage), kw_(kw) {}

// 伪代码：
// 步骤1：深度钳位到 [2,3]；初始化结果表 result、当前波前 cur、已扩展集合 expanded。
// 步骤2：装入种子——若种子节点在库中不存在则跳过；否则把 weight 累加进
//        result 并生成一条单点路径，同时以 hop=0 加入当前波前。
// 步骤3：逐层扩散循环，直到波前为空：
//   3.1 对波前中每个节点 f：若 hop 已达深度上限则跳过；若已扩展节点数达到
//       budget 则整层停止扩展；若 f 已扩展过则跳过，否则标记并计数。
//   3.2 遍历 f 的邻居（可按 rel_types 过滤通道）：
//       - 若邻居是休眠关键词（Keyword 且 status==1）则跳过；
//       - 计算边传导 trans = 类型系数 × 边权 × 类型乘子（weights 空则乘 1）；
//         若边权 > 5 则按 5 + log1p(w-5) 软化高频边；
//       - 子能量 child_e = f.energy × trans × (hop+1)/(hop+2)，即叠加
//         1/(hop+1) 衰减的增量因子；child_e < 0.01 视为枯竭，剪枝；
//       - 在 f 的路径上追加本步边，更新路径能量；
//       - 把 child_e 累加进邻居的结果能量，路径插入 paths 后按能量降序
//         排序并截断到 top3；
//       - 若该邻居在本层尚未入队且下一跳不超深度，则加入下一层波前
//         （同层去重，但能量与路径可多次累加）。
//   3.3 下一层波前接替当前波前，回到 3。
// 步骤4：返回 result（node_uid -> 能量 + top3 路径）。
DiffusionResult DiffusionEngine::diffuse(const std::vector<DiffusionSeed>& seeds, int depth,
                                         const std::vector<RelType>* rel_types,
                                         const std::unordered_map<int, double>* weights,
                                         int budget) const {
  return diffuse_beam(seeds, depth, rel_types, weights, budget, 0, nullptr);  // beam=0 即原 BFS
}

// 伪代码（束搜索 best-first，引导式扩散阶段 1）：
// 与 diffuse 同流程，仅两处扩展：
//   ① beam 剪枝：beam_width>0 时每层先把波前按能量降序截断到 beam_width；
//   ② 边传导分：gate_fn 非空时由门控函数给出（监督边门控），否则规则传导。
// 能量衰减/路径记录/top3/budget/休眠隔离语义与 diffuse 完全一致。
DiffusionResult DiffusionEngine::diffuse_beam(const std::vector<DiffusionSeed>& seeds,
                                              int depth,
                                              const std::vector<RelType>* rel_types,
                                              const std::unordered_map<int, double>* weights,
                                              int budget, int beam_width,
                                              const GateFn& gate_fn) const {
  if (depth < 2) depth = 2;  // 深度硬限 2~3
  if (depth > 3) depth = 3;

  DiffusionResult result;
  struct Frontier {
    std::string uid;
    double energy;   // 到达该节点的能量（已含 1/(hop+1)）
    int hop;
    Path path;
  };
  std::vector<Frontier> cur;
  std::unordered_set<std::string> expanded;
  int expanded_count = 0;

  for (auto& s : seeds) {
    if (!storage_.get_node(s.node_uid)) continue;
    auto& ne = result[s.node_uid];
    ne.energy += s.weight;
    Path p; p.energy = s.weight;
    ne.paths.push_back(p);
    cur.push_back({s.node_uid, s.weight, 0, p});
  }

  while (!cur.empty()) {
    if (beam_width > 0 && (int)cur.size() > beam_width) {  // beam 剪枝：top-m 前沿
      std::sort(cur.begin(), cur.end(),
                [](const Frontier& a, const Frontier& b) { return a.energy > b.energy; });
      cur.resize(beam_width);
    }
    std::vector<Frontier> next;
    for (auto& f : cur) {
      if (f.hop >= depth) continue;
      if (expanded_count >= budget) break;  // budget 限制扩展节点数
      if (expanded.count(f.uid)) continue;
      expanded.insert(f.uid);
      ++expanded_count;

      auto nbs = storage_.neighbors(f.uid, rel_types);
      // PageRank 式出度归一：枢纽节点（如主角实体，邻边数百条）的能量摊薄到每条边，
      // 防止"种子在高产节点上均匀喷灌"淹没直接证据（通用机制，对应设计文档"扩散爆炸：
      // 深度/密度双限"）。
      double deg = (double)nbs.size();
      const Node* head = storage_.get_node(f.uid);
      for (auto& [e, nb] : nbs) {
        // 休眠关键词不参与扩散
        if (nb->kind == NodeKind::Keyword && nb->status == 1) continue;
        double trans;
        if (gate_fn && head) {
          trans = gate_fn(e, *head, *nb, f.hop);  // 监督边门控传导
        } else {
          double mult = 1.0;
          if (weights) {
            auto it = weights->find((int)e.type);
            if (it != weights->end()) mult = it->second;
          }
          trans = conduction_coeff(e.type) * e.weight * mult;  // 边传导（规则）
          if (e.weight > 5.0) trans = conduction_coeff(e.type) * (5.0 + std::log1p(e.weight - 5.0)) * mult;  // 高频边软化
        }
        double child_e = f.energy * trans * (double)(f.hop + 1) / (double)(f.hop + 2) / deg;
        // 能量按 1/(hop+1) 衰减：hop+1 跳处相对 hop 处的增量因子；按出度摊薄
        if (child_e < 0.01) continue;  // 能量枯竭剪枝

        Path p = f.path;
        p.steps.push_back({f.uid, nb->uid, e.type});
        p.energy = child_e;

        auto& ne = result[nb->uid];
        bool seen = false;  // 同一节点同一 hop 内避免重复入队
        for (auto& x : next) if (x.uid == nb->uid) { seen = true; break; }
        ne.energy += child_e;
        ne.paths.push_back(p);
        std::sort(ne.paths.begin(), ne.paths.end(),
                  [](const Path& a, const Path& b) { return a.energy > b.energy; });
        if (ne.paths.size() > 3) ne.paths.resize(3);  // 每节点保留 top3 路径
        if (!seen && f.hop + 1 <= depth) next.push_back({nb->uid, child_e, f.hop + 1, p});
      }
    }
    cur = std::move(next);
  }
  return result;
}

}  // namespace ame
