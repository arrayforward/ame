add_executable(ame_demo demo_main.cpp)
target_link_libraries(ame_demo PRIVATE ame_engine)

# E2E 场景验证器（直调 AME + MiniMax API；需 android/local.properties 配置）
add_executable(ame_e2e_summary e2e_summary.cpp)
target_link_libraries(ame_e2e_summary PRIVATE ame_engine)
