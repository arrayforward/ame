add_library(ame_feedback STATIC src/feedback_collector.cpp)
target_include_directories(ame_feedback PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_feedback PUBLIC ame_core)
