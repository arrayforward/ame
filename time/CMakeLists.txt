add_library(ame_time STATIC src/time_service.cpp)
target_include_directories(ame_time PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_time PUBLIC ame_core ame_storage)
