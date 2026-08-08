add_library(ame_keyword STATIC src/keyword_manager.cpp)
target_include_directories(ame_keyword PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_keyword PUBLIC ame_core ame_storage)
