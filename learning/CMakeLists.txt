add_library(ame_learning STATIC src/learning_engine.cpp src/gate_model.cpp)
target_include_directories(ame_learning PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_learning PUBLIC ame_core ame_storage ame_vector ame_rerank ame_feedback ame_time)
