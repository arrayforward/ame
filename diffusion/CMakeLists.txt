add_library(ame_diffusion STATIC src/diffusion_engine.cpp)
target_include_directories(ame_diffusion PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_diffusion PUBLIC ame_core ame_storage ame_keyword)
