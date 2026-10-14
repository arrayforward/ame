add_library(ame_mem STATIC src/mem.cpp)
target_include_directories(ame_mem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_mem PUBLIC
  ame_core ame_memobs ame_memfact ame_memindex ame_memview)
