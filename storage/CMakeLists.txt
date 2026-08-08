add_library(ame_storage STATIC src/storage.cpp)
target_include_directories(ame_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_storage PUBLIC ame_core)
