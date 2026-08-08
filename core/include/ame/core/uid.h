/*
 * 所属模块：core 公共基础层——语义无关短随机 uid 生成。
 * 本文件主体思路：对外提供 gen_uid() 生成 "E + 8 位 base32" 短码
 *   （发出不改、永不复用），以及 base32_encode40() 将 64bit 值的低 40bit
 *   编码为 8 字符（crockford 小写无歧义字母表）。
 * 关键算法/数据结构：base32 编码；随机性来源（splitmix64 混合）见 uid.cpp。
 * 依赖关系：仅依赖标准库；被需要新建节点标识的模块（graph_writer M4、storage M9 等）使用。
 */
#pragma once
#include <string>

namespace ame {

// 生成形如 "E7KQ3X9MA" 的短码（E 前缀 + 8 位 base32）
std::string gen_uid();

// base32(crockford 小写无歧义字母表) 编码 64bit 值的低 40bit -> 8 字符
std::string base32_encode40(uint64_t v);

}  // namespace ame
