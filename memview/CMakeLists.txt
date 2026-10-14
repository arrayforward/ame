add_library(ame_memview STATIC src/memview.cpp)
target_include_directories(ame_memview PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_memview PUBLIC ame_core ame_memobs ame_memfact ame_memindex)
