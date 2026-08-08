/*
 * core 单元测试：uid 生成唯一性、RelType 传导系数、JSON 序列化往返
 */
#include <unordered_set>

#include "ame/core/json.h"
#include "ame/core/types.h"
#include "ame/core/uid.h"
#include "test_util.hpp"

using namespace ame;

static void test_uid_uniqueness() {
  std::unordered_set<std::string> seen;
  for (int i = 0; i < 10000; ++i) {
    std::string u = gen_uid();
    CHECK(u.size() == 9 && u[0] == 'E');
    CHECK(seen.insert(u).second);
  }
}

static void test_conduction_coeff() {
  CHECK_NEAR(conduction_coeff(RelType::REL_CAUSE), 0.9, 1e-9);
  CHECK_NEAR(conduction_coeff(RelType::REL_INFER), 0.9, 1e-9);
  CHECK_NEAR(conduction_coeff(RelType::REL_PART), 0.8, 1e-9);
  CHECK_NEAR(conduction_coeff(RelType::REL_SIMILAR), 0.7, 1e-9);
  CHECK_NEAR(conduction_coeff(RelType::REL_COOCCUR), 0.6, 1e-9);
  CHECK_NEAR(conduction_coeff(RelType::REL_OPPOSITE), 0.5, 1e-9);
}

static void test_rel_name_roundtrip() {
  for (int i = 0; i <= (int)RelType::LIFE_PHASE; ++i) {
    RelType t = (RelType)i;
    auto back = rel_from_name(rel_name(t));
    CHECK(back.has_value() && *back == t);
  }
  CHECK(!rel_from_name("NO_SUCH_REL").has_value());
  CHECK(rel_undirected(RelType::NEAR));
  CHECK(rel_undirected(RelType::REL_COOCCUR));
  CHECK(!rel_undirected(RelType::INVOLVES));
}

static void test_json_roundtrip() {
  Json j = Json::object();
  j["name"] = Json::string("老张@朝阳公园");
  j["n"] = Json::number(42);
  j["f"] = Json::number(0.5);
  j["flag"] = Json::boolean(true);
  Json arr = Json::array();
  arr.push(Json::number(1));
  arr.push(Json::string("失眠"));
  j["arr"] = std::move(arr);

  bool ok = false;
  Json back = Json::parse(j.dump(), &ok);
  CHECK(ok);
  CHECK(back.find("name")->as_str() == "老张@朝阳公园");
  CHECK(back.find("n")->as_num() == 42);
  CHECK(back.find("f")->as_num() == 0.5);
  CHECK(back.find("flag")->as_bool());
  CHECK(back.find("arr")->arr.size() == 2);
  CHECK(back.find("arr")->arr[1].as_str() == "失眠");

  bool ok2 = true;
  Json::parse("{broken json", &ok2);
  CHECK(!ok2);
}

int main() {
  test_uid_uniqueness();
  test_conduction_coeff();
  test_rel_name_roundtrip();
  test_json_roundtrip();
  return AME_TEST_REPORT();
}
