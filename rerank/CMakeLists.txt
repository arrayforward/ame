add_library(ame_rerank STATIC src/reranker.cpp)
target_include_directories(ame_rerank PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_rerank PUBLIC ame_core ame_storage ame_time ame_vector ame_diffusion)
