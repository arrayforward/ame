add_library(ame_llm STATIC
  src/client.cpp
  src/components.cpp
  src/lexicon.cpp
)
target_include_directories(ame_llm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_llm PUBLIC ame_core ame_extractor ame_bench)
