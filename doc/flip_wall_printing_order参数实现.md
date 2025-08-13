# flip_wall_printing_order参数实现

## 功能概述

新增`flip_wall_printing_order`参数，用于控制同一层墙体内多个多边形的打印顺序。

### 应用场景

对于圆筒等具有内外两圈墙体的模型：
- **默认顺序**：从内到外打印（内圈→外圈）
- **flip_wall_printing_order=true**：从外到内打印（外圈→内圈）

## 技术实现

### 核心原理

利用`PathOrderOptimizer`的`reverse_direction`参数来控制多边形的打印顺序：

```cpp
// === 应用flip_wall_printing_order参数：控制同一层墙体内多边形的打印顺序 ===
const bool flip_wall_printing_order = settings_.get<bool>("flip_wall_printing_order");
const bool reverse_polygon_order = flip_wall_printing_order;

PathOrderOptimizer<const ExtrusionLine*> order_optimizer(
    gcode_layer_.getLastPlannedPositionOrStartingPosition(),
    z_seam_config_,
    detect_loops,
    combing_boundary,
    reverse_polygon_order,  // 使用flip_wall_printing_order控制多边形顺序
    order,
    group_outer_walls,
    disallowed_areas_for_seams_,
    use_shortest_for_inner_walls,
    overhang_areas_);
```

### 参数区别

#### inset_direction vs flip_wall_printing_order

| 参数 | 控制范围 | 作用 |
|------|----------|------|
| `inset_direction` | wall0与wallx之间 | 控制外墙和内墙的相对顺序 |
| `flip_wall_printing_order` | 同一层内多边形 | 控制同一层墙体内多个多边形的顺序 |

#### 示例：圆筒模型

```
圆筒模型有两个多边形：
- 外圈多边形（大圆）
- 内圈多边形（小圆）

flip_wall_printing_order=false（默认）：
1. 内圈多边形（小圆）
2. 外圈多边形（大圆）

flip_wall_printing_order=true：
1. 外圈多边形（大圆）  
2. 内圈多边形（小圆）
```

### PathOrderOptimizer机制

#### reverse_direction参数作用

在`PathOrderOptimizer::optimize()`中：

```cpp
if (reverse_direction_ && order_requirements_->empty())
{
    std::vector<OrderablePath> reversed = reverseOrderPaths(optimized_order);
    std::swap(reversed, paths_);
}
```

#### reverseOrderPaths函数

```cpp
std::vector<OrderablePath> reverseOrderPaths(std::vector<OrderablePath> pathsOrderPaths)
{
    std::vector<OrderablePath> reversed;
    reversed.reserve(pathsOrderPaths.size());
    for (auto& path : pathsOrderPaths | ranges::views::reverse)
    {
        reversed.push_back(path);
    }
    return reversed;
}
```

完全反转多边形的打印顺序。

## 调试信息

### 调试日志

```cpp
CURA_DEBUG(WALL_COMPUTATION, "flip_wall_printing_order={}, 控制同一层墙体内多边形的打印顺序", flip_wall_printing_order);
```

### 启用调试

```bash
# 启用墙体计算调试信息
./CuraEngine --debug-only WALL_COMPUTATION [其他参数]
```

### 预期输出

```
[DEBUG] flip_wall_printing_order=true, 控制同一层墙体内多边形的打印顺序
[DEBUG] InsetOrderOptimizer使用combing边界，避免穿越模型的travel路径
```

## 参数定义

### CuraEngine端

参数已在代码中使用：
```cpp
const bool flip_wall_printing_order = settings_.get<bool>("flip_wall_printing_order");
```

### Cura前端配置

✅ **已完成**：在fdmprinter.def.json中更新参数定义：

```json
{
    "flip_wall_printing_order": {
        "label": "Flip Wall Printing Order",
        "description": "Reverse the printing order of polygons within the same wall layer. For hollow models like cylinders, this controls whether to print outer ring first or inner ring first. This is different from 'Wall Ordering' which controls the order between wall_0 and wall_x.",
        "type": "bool",
        "default_value": false,
        "settable_per_mesh": true,
        "settable_per_extruder": true
    }
}
```

### 多语言翻译

✅ **已完成**：按照参数规范要求，在i18n中添加了完整翻译：

#### 简体中文 (zh_CN)
```po
msgctxt "flip_wall_printing_order label"
msgid "Flip Wall Printing Order"
msgstr "翻转墙体打印顺序"

msgctxt "flip_wall_printing_order description"
msgid "Reverse the printing order of polygons within the same wall layer. For hollow models like cylinders, this controls whether to print outer ring first or inner ring first. This is different from 'Wall Ordering' which controls the order between wall_0 and wall_x."
msgstr "反转同一层墙体内多边形的打印顺序。对于圆筒等空心模型，控制是先打印外圈还是先打印内圈。这与控制wall_0和wall_x之间顺序的"墙体顺序"不同。"
```

#### 繁体中文 (zh_TW)
```po
msgctxt "flip_wall_printing_order label"
msgid "Flip Wall Printing Order"
msgstr "翻轉牆體列印順序"

msgctxt "flip_wall_printing_order description"
msgid "Reverse the printing order of polygons within the same wall layer. For hollow models like cylinders, this controls whether to print outer ring first or inner ring first. This is different from 'Wall Ordering' which controls the order between wall_0 and wall_x."
msgstr "反轉同一層牆體內多邊形的列印順序。對於圓筒等空心模型，控制是先列印外圈還是先列印內圈。這與控制wall_0和wall_x之間順序的「牆體順序」不同。"
```

#### 日语 (ja_JP)
```po
msgctxt "flip_wall_printing_order label"
msgid "Flip Wall Printing Order"
msgstr "ウォール印刷順序を反転"

msgctxt "flip_wall_printing_order description"
msgid "Reverse the printing order of polygons within the same wall layer. For hollow models like cylinders, this controls whether to print outer ring first or inner ring first. This is different from 'Wall Ordering' which controls the order between wall_0 and wall_x."
msgstr "同一ウォール層内のポリゴンの印刷順序を反転します。円筒などの中空モデルでは、外側のリングを先に印刷するか内側のリングを先に印刷するかを制御します。これはwall_0とwall_xの間の順序を制御する「ウォール順序」とは異なります。"
```

## 使用示例

### 命令行使用

```bash
# 默认顺序（内圈→外圈）
./CuraEngine slice -j settings.json -o output.gcode -l model.stl

# 翻转顺序（外圈→内圈）
./CuraEngine slice -j settings.json -s flip_wall_printing_order=true -o output.gcode -l model.stl
```

### 配置文件

```json
{
    "flip_wall_printing_order": true,
    "wall_line_count": 2,
    "optimize_wall_printing_order": true
}
```

## 兼容性

### 向后兼容

- **默认值**：`false`，保持现有行为
- **现有模型**：不受影响，除非显式启用
- **其他参数**：与现有墙体参数完全兼容

### 参数交互

#### 与optimize_wall_printing_order

- `optimize_wall_printing_order=false`：按inset分组打印，flip_wall_printing_order仍然有效
- `optimize_wall_printing_order=true`：按区域优化打印，flip_wall_printing_order控制区域内顺序

#### 与inset_direction

- 两个参数独立工作，控制不同层面的顺序
- 可以同时使用，实现精细的打印顺序控制

## 性能影响

### 计算开销

- **额外开销**：几乎为零，只是改变参数传递
- **内存使用**：无额外内存消耗
- **优化时间**：不影响路径优化算法的性能

### 打印质量

#### 潜在优势

1. **减少回抽**：某些几何形状下可能减少travel距离
2. **温度控制**：改变打印顺序可能影响冷却效果
3. **支撑效果**：外圈先打印可能提供更好的支撑

#### 注意事项

1. **粘附性**：顺序改变可能影响层间粘附
2. **精度**：不同顺序可能影响尺寸精度
3. **材料特性**：某些材料对打印顺序敏感

## 测试验证

### 测试模型

1. **圆筒模型**：验证内外圈顺序控制
2. **复杂几何**：多个独立多边形的模型
3. **嵌套结构**：多层嵌套的复杂模型

### 验证方法

1. **G代码分析**：检查travel路径和打印顺序
2. **可视化工具**：使用Cura预览功能观察路径
3. **实际打印**：验证打印质量和效果

## 总结

`flip_wall_printing_order`参数提供了对同一层墙体内多边形打印顺序的精细控制：

1. **功能明确**：专门控制多边形顺序，不影响其他功能
2. **实现简洁**：利用现有PathOrderOptimizer机制
3. **兼容性好**：向后兼容，不破坏现有功能
4. **调试友好**：提供详细的调试信息
5. **性能优秀**：几乎无额外性能开销

这个功能为用户提供了更多的打印策略选择，特别适用于圆筒、管道等空心结构的优化打印。
