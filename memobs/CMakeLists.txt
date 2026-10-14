add_library(ame_memobs STATIC src/memobs.cpp)
target_include_directories(ame_memobs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_memobs PUBLIC ame_core)
