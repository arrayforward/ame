add_library(ame_entity STATIC src/entity_resolver.cpp)
target_include_directories(ame_entity PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_entity PUBLIC ame_core ame_storage ame_vector)
