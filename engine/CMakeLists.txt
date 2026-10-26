add_library(ame_engine STATIC src/engine.cpp)
target_include_directories(ame_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_engine PUBLIC
  ame_core ame_storage ame_time ame_vector
  ame_keyword ame_extractor ame_entity ame_graph_writer
  ame_seed ame_diffusion ame_rerank
)
