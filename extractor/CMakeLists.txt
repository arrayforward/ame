add_library(ame_extractor STATIC src/extractor.cpp src/ac_automaton.cpp src/dat_trie.cpp)
target_include_directories(ame_extractor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_extractor PUBLIC ame_core ame_time)
