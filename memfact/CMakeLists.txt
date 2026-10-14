add_library(ame_memfact STATIC src/memfact.cpp)
target_include_directories(ame_memfact PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_memfact PUBLIC ame_core)
