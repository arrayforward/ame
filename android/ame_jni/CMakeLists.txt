# 所属模块：android/ame_jni——AME 引擎的 Android JNI 封装构建脚本。
# 本文件主体思路：以 add_subdirectory 复用桌面端各模块的 CMakeLists（源码零分叉），
#   只编引擎侧 15 个模块为静态库，链接进 libame_jni.so；llm/bench/tests/demo 不进 Android
#   （联网调用在 Kotlin 侧 OkHttp 完成）。
# 关键算法/数据结构：无；纯构建编排。
# 依赖关系：依赖仓库根各模块目录（相对路径 ../../<module>）；由 app/build.gradle 的
#   externalNativeBuild 调用，NDK 工具链由 AGP 注入。
cmake_minimum_required(VERSION 3.22.1)
project(ame_jni CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# AME 源码根（android/ame_jni → 仓库根）
set(AME_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# 引擎侧模块（与桌面端共用同一份源码；顺序无关，CMake 自动按依赖排序）
# 1.x 模块（bench/llm/tests/demo 不进 Android；2.0 分层记忆 M15–M19 已随 3.0 刀1 裁剪）
set(AME_MODULES core storage time vector extractor entity keyword
                graph_writer seed diffusion rerank engine)
foreach(m ${AME_MODULES})
  add_subdirectory(${AME_ROOT}/${m} ${CMAKE_BINARY_DIR}/ame/${m})
endforeach()

add_library(ame_jni SHARED ame_jni.cpp)
target_link_libraries(ame_jni PRIVATE ame_engine android log)
