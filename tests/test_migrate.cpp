/*
 * AME 2.0 P1 迁移工具单测（设计 §10 P1：1.x graph.json → 观测层迁移）
 *   ① Event 节点逐字段转观测：kind 由 attrs.speaker 映射（user/assistant/缺失），
 *     text/ts 原样，attrs.dia_id 进 meta，非 Event 节点与空 text 事件跳过；
 *   ② 幂等：二次迁移返回 0、观测数不变（迁移中断重启重做不重复——按
 *     channel+kind+ts+text 去重，因观测 seq 单调无法靠 seq 判重）；
 *   ③ migrated.flag 约定：migrate_flag_path/migrate_done 与 JNI 同一判据，
 *     写标记后视为已迁；同 root 重开 Mem replay 恢复观测（重启持久化）。
 *   ④ 容错：文件缺失/解析失败/裸图快照（无 "graph" 包装）。
 */
#include "ame/mem/mem.h"
#include "test_util.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ame;

// 临时持久化根目录（每个用例独立，先清后建）
static std::string temp_root(const std::string& name) {
  const std::string p = (std::filesystem::temp_directory_path() / name).string();
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
  return p;
}

static void write_file(const std::string& path, const std::string& content) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << content;
}

// 收集观测的轻量视图（字段断言用）
struct Got {
  std::string kind, text, dia_id;
  int64_t ts = 0;
};
static std::vector<Got> collect(Mem& m, const std::string& owner,
                                const std::string& channel) {
  std::vector<Got> out;
  m.obs_store().for_channel(owner, channel, [&](const Observation& o) {
    Got g;
    g.kind = o.kind;
    g.text = o.text;
    g.ts = o.ts;
    const Json* d = o.meta.find("dia_id");
    g.dia_id = d ? d->as_str() : "";
    out.push_back(std::move(g));
  });
  return out;
}

// 1.x Engine 组合快照（graph 包装）：3 个有效事件 + 1 空 text + 1 非 Event
static const char* kGraphJson = R"({
  "graph": {
    "nodes": [
      {"uid":"e1","kind":0,"name":"","text":"用户说想养一只猫","type":"","ts":1700000000,
       "precision":3,"importance":0.5,"lat":0,"lng":0,"geohash":"","status":0,
       "mention_count":0,
       "attrs":{"speaker":"user","dia_id":"u1","session_id":"s1"}},
      {"uid":"e2","kind":0,"name":"","text":"助手推荐橘猫","type":"","ts":1700000060,
       "precision":3,"importance":0.5,"lat":0,"lng":0,"geohash":"","status":0,
       "mention_count":0,
       "attrs":{"speaker":"assistant","dia_id":"a1","session_id":"s1"}},
      {"uid":"e3","kind":0,"name":"","text":"没有 attrs 的事件","type":"","ts":1700000120,
       "precision":3,"importance":0.5,"lat":0,"lng":0,"geohash":"","status":0,
       "mention_count":0},
      {"uid":"e4","kind":0,"name":"","text":"","type":"","ts":1700000180,
       "precision":3,"importance":0.5,"lat":0,"lng":0,"geohash":"","status":0,
       "mention_count":0,
       "attrs":{"speaker":"user","dia_id":"u4"}},
      {"uid":"n1","kind":1,"name":"猫","text":"","type":"keyword","ts":0,
       "precision":3,"importance":0.5,"lat":0,"lng":0,"geohash":"","status":0,
       "mention_count":0}
    ],
    "edges": []
  },
  "slots": [],
  "aliases": []
})";

// ---------------- ① 字段级迁移正确性 ----------------
static void test_migrate_fields() {
  const std::string session = temp_root("ame_migrate_fields/session");
  const std::string lmem = session + "/lmem";
  write_file(session + "/graph.json", kGraphJson);

  Mem m(lmem);
  const size_t n = migrate_graph(m, session + "/graph.json", "s1", "s1");
  CHECK(n == 3);  // e1/e2/e3 入库；e4 空 text、n1 非 Event 跳过

  const auto got = collect(m, "s1", "s1");
  CHECK(got.size() == 3);

  // 逐字段：speaker 映射 kind，text/ts 原样，dia_id 进 meta
  CHECK(got[0].kind == "user");
  CHECK(got[0].text == "用户说想养一只猫");
  CHECK(got[0].ts == 1700000000);
  CHECK(got[0].dia_id == "u1");
  CHECK(got[1].kind == "assistant");
  CHECK(got[1].text == "助手推荐橘猫");
  CHECK(got[1].ts == 1700000060);
  CHECK(got[1].dia_id == "a1");

  // 容错：缺 attrs 的事件 → assistant、无 dia_id
  CHECK(got[2].kind == "assistant");
  CHECK(got[2].text == "没有 attrs 的事件");
  CHECK(got[2].ts == 1700000120);
  CHECK(got[2].dia_id.empty());
}

// ---------------- ② 幂等 + ③ flag 约定 + 重启 replay ----------------
static void test_migrate_idempotent_flag() {
  const std::string session = temp_root("ame_migrate_flag/session");
  const std::string lmem = session + "/lmem";
  write_file(session + "/graph.json", kGraphJson);

  CHECK(!migrate_done(lmem));  // 未迁过

  {
    Mem m(lmem);
    CHECK(migrate_graph(m, session + "/graph.json", "s1", "s1") == 3);
    // 中断场景：观测已写入但 flag 未写 → 重启重做必须不重复
    CHECK(migrate_graph(m, session + "/graph.json", "s1", "s1") == 0);
    CHECK(collect(m, "s1", "s1").size() == 3);
  }
  // 新实例模拟重启：分片 replay 恢复，观测数仍 3（seq 单调续接不重复）
  {
    Mem m(lmem);
    CHECK(collect(m, "s1", "s1").size() == 3);
    CHECK(migrate_graph(m, session + "/graph.json", "s1", "s1") == 0);
    CHECK(collect(m, "s1", "s1").size() == 3);
  }

  // flag 约定（与 JNI nativeCreate 同一判据）：写标记 → 视为已迁
  write_file(migrate_flag_path(lmem), "ok\n");
  CHECK(migrate_done(lmem));
  {
    Mem m(lmem);  // JNI 侧此时直接跳过迁移分支
    CHECK(collect(m, "s1", "s1").size() == 3);
  }
}

// ---------------- ④ 容错：缺文件/坏 JSON/裸图快照 ----------------
static void test_migrate_fault_tolerance() {
  const std::string session = temp_root("ame_migrate_fault/session");
  const std::string lmem = session + "/lmem";

  Mem m(lmem);
  // 文件不存在：静默 0（JNI 侧 precondition 已判 exists，双保险）
  CHECK(migrate_graph(m, session + "/graph.json", "s1", "s1") == 0);
  // 解析失败：0
  write_file(session + "/graph.json", "{not json");
  CHECK(migrate_graph(m, session + "/graph.json", "s1", "s1") == 0);
  // 无 nodes 键：0
  write_file(session + "/graph.json", "{\"graph\":{\"edges\":[]}}");
  CHECK(migrate_graph(m, session + "/graph.json", "s1", "s1") == 0);

  // 裸图快照（无 "graph" 包装，兼容旧格式）
  const std::string root2 = temp_root("ame_migrate_bare/session") ;
  const std::string lmem2 = root2 + "/lmem";
  write_file(root2 + "/graph.json", R"({"nodes":[
      {"uid":"e1","kind":0,"text":"裸快照事件","ts":1700001000,
       "attrs":{"speaker":"user","dia_id":"u9"}}
    ],"edges":[]})");
  Mem m2(lmem2);
  CHECK(migrate_graph(m2, root2 + "/graph.json", "s1", "s1") == 1);
  const auto got = collect(m2, "s1", "s1");
  CHECK(got.size() == 1);
  CHECK(got[0].text == "裸快照事件");
  CHECK(got[0].dia_id == "u9");
}

int main() {
  test_migrate_fields();
  test_migrate_idempotent_flag();
  test_migrate_fault_tolerance();
  return AME_TEST_REPORT();
}
