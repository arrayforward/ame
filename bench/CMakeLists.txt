add_library(ame_bench STATIC
  src/loader.cpp
  src/ingest_answer.cpp
  src/metrics.cpp
  src/lexicon_apply.cpp
  src/store.cpp
)
target_include_directories(ame_bench PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_bench PUBLIC ame_engine ame_time ame_mem)

add_executable(ame_bench_cli cli/main.cpp)
target_link_libraries(ame_bench_cli PRIVATE ame_bench ame_llm)
set_target_properties(ame_bench_cli PROPERTIES OUTPUT_NAME ame_bench)
