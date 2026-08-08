add_library(ame_seed STATIC src/seed_generator.cpp)
target_include_directories(ame_seed PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_seed PUBLIC ame_core ame_storage ame_vector ame_keyword ame_extractor)
