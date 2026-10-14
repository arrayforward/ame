add_library(ame_memindex STATIC src/memindex.cpp)
target_include_directories(ame_memindex PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_memindex PUBLIC ame_core ame_memobs ame_memfact)
