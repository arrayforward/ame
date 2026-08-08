add_library(ame_core STATIC
  src/types.cpp
  src/uid.cpp
  src/json.cpp
  src/log.cpp
)
target_include_directories(ame_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(ame_core PUBLIC cxx_std_17)
