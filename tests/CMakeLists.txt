# AME 测试：每文件一个可执行 + ctest 注册
# test_util.hpp 提供共享极简断言宏（CHECK / CHECK_NEAR / AME_TEST_REPORT）
set(AME_TESTS
  test_core          # core：uid/系数/JSON
  test_storage       # M9  邻接表/快照/geo
  test_time          # M10 地方时投影/衰减/人生阶段
  test_vector        # M13 哈希 embedding/compose/ANN
  test_extractor     # M1  规则提取
  test_entity        # M2  三级定位/merge/split/别名
  test_keyword       # M3  归一/休眠/RELATED
  test_graph_writer  # M4  写入即生长/摘要聚合
  test_seed          # M5  硬/软/坐标/文本种子
  test_diffusion     # M6  能量/路径/休眠隔离/budget
  test_rerank        # M7  融合打分/filters
  test_thinker       # M8  多轮 ReasoningChain
  test_engine        # M12 网关动词/错误码
  test_e2e           # 端到端全链路
)

foreach(t ${AME_TESTS})
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} PRIVATE ame_engine)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# bench 基准适配层测试（依赖 ame_bench 与夹具目录）
add_executable(test_bench test_bench.cpp)
target_link_libraries(test_bench PRIVATE ame_bench)
target_compile_definitions(test_bench PRIVATE AME_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/bench/fixtures")
add_test(NAME test_bench COMMAND test_bench)

# llm 接入层测试（mock 模式，不打真实 API）
add_executable(test_llm test_llm.cpp)
target_link_libraries(test_llm PRIVATE ame_llm ame_bench ame_engine)
add_test(NAME test_llm COMMAND test_llm)
