add_library(ame_thinker STATIC src/thinker.cpp)
target_include_directories(ame_thinker PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_thinker PUBLIC ame_core ame_storage ame_vector ame_seed ame_diffusion ame_rerank)
