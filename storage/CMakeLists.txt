add_library(ame_storage STATIC src/storage.cpp)
target_include_directories(ame_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ame_storage PUBLIC ame_core)

# 4.0：PostgreSQL 后端（默认 OFF；ON 时 libpq 必装，检索下推见 storage/pg/）
if(AME_WITH_POSTGRES)
  find_package(PostgreSQL REQUIRED)
  target_sources(ame_storage PRIVATE pg/pg_backend.cpp)
  target_compile_definitions(ame_storage PUBLIC AME_WITH_POSTGRES)
  target_link_libraries(ame_storage PUBLIC PostgreSQL::PostgreSQL)
endif()
