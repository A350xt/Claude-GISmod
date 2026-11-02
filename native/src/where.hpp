// query.filter 的 where 表达式：pandas query 语法的常用子集。
//
//   比较      ==  !=  <  <=  >  >=（可链式：100 < pop <= 500）
//   逻辑      and / or / not，以及 & | ~（与 pandas 一样按 and/or/not 的优先级处理）
//   成员      landuse in ['商业', '居住']   /   class not in ("高速",)
//   算术      + - * / % **，一元负号
//   列名      普通标识符（含中文）或 `反引号` 包裹（关键字/含空格的列名）
//   字面量    数字、'文本' / "文本"、True、False、None
//   时间      时间列可直接与 ISO 文本比较：time >= "2024-06-01"
//   方法      col.str.contains('x') / .str.startswith / .str.endswith / .str.lower() / .str.upper()
//             col.isna() / col.notna()（别名 isnull / notnull）
//
// 空值语义与 pandas 一致：空值参与的比较为 False（!= 为 True）。
#pragma once

#include <string>
#include <vector>

#include "layer.hpp"

namespace gis {

/// 返回满足 where 的行下标；语法或类型错误抛 E_BAD_REQUEST
std::vector<size_t> filter_rows(const Layer& layer, const std::string& where);

}  // namespace gis
