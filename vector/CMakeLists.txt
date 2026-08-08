add_library(ame_vector STATIC src/vector_service.cpp)
target_include_directories(ame_vector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_vector PUBLIC ame_core ame_storage)
