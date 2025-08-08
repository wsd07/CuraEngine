# BeadingStrategy 系统

## 概述

BeadingStrategy 系统是 CuraEngine 中负责计算墙体线宽和路径的核心组件。它通过复杂的算法计算最优的线宽分布，以实现更好的打印质量和精度。

## 核心功能

### 1. BeadingStrategy 系统架构

**主要组件**:
- `BeadingStrategy`: 抽象基类
- `DistributedBeadingStrategy`: 分布式线宽策略
- `RedistributeBeadingStrategy`: 重分布线宽策略
- `LimitedBeadingStrategy`: 限制线宽策略

### 2. 传统简单偏移算法

**问题**: BeadingStrategy 系统过于复杂，某些场景下用户希望使用简单的偏移算法。

**解决方案**: 添加 `beading_strategy_enable` 参数，允许完全禁用 BeadingStrategy 系统。

## 实现方案

### 1. 参数定义

**文件位置**: `fdmprinter.def.json`

```json
"beading_strategy_enable": {
    "label": "Enable Beading Strategy",
    "description": "Enable the advanced beading strategy system for wall width calculation. When disabled, uses simple offset algorithm instead.",
    "type": "bool",
    "default_value": true,
    "settable_per_mesh": true,
    "settable_per_extruder": true
}
```

### 2. 核心实现

**文件位置**: `CuraEngine/src/WallsComputation.cpp`

```cpp
void WallsComputation::generateWalls(SliceLayerPart* part, SectionType section_type)
{
    const bool beading_strategy_enable = settings_.get<bool>("beading_strategy_enable");
    
    if (!beading_strategy_enable) {
        // 使用简单偏移算法
        generateWallsWithSimpleOffset(part, section_type);
        return;
    }
    
    // 使用 BeadingStrategy 系统（原有逻辑）
    generateWallsWithBeadingStrategy(part, section_type);
}

void WallsComputation::generateWallsWithSimpleOffset(SliceLayerPart* part, SectionType section_type)
{
    const coord_t line_width_0 = settings_.get<coord_t>("wall_line_width_0");
    const coord_t line_width_x = settings_.get<coord_t>("wall_line_width_x");
    const size_t wall_count = settings_.get<size_t>("wall_line_count");
    
    spdlog::debug("【简单偏移】使用简单偏移算法生成墙体，墙体数量: {}", wall_count);
    
    // 生成外墙 (wall_0)
    if (wall_count > 0) {
        coord_t wall_0_inset = line_width_0 / 2;
        Shape wall_0_outline = part->outline.offset(-wall_0_inset);
        
        if (!wall_0_outline.empty()) {
            part->wall_toolpaths.emplace_back(
                VariableWidthLines(), 
                0, // inset_idx
                true // is_outer_wall
            );
            
            // 转换为 VariableWidthLines
            for (const Polygon& poly : wall_0_outline) {
                ExtrusionLine line;
                for (const Point2LL& point : poly) {
                    line.emplace_back(point, line_width_0, 1);
                }
                if (!line.empty()) {
                    part->wall_toolpaths.back().toolpaths.push_back(line);
                }
            }
        }
    }
    
    // 生成内墙 (wall_x)
    Shape current_outline = part->outline;
    for (size_t wall_idx = 0; wall_idx < wall_count; wall_idx++) {
        coord_t inset_distance = (wall_idx == 0) ? 
            line_width_0 : 
            line_width_0 + wall_idx * line_width_x;
        
        Shape wall_outline = part->outline.offset(-inset_distance);
        
        if (!wall_outline.empty()) {
            if (wall_idx > 0) { // 内墙
                part->wall_toolpaths.emplace_back(
                    VariableWidthLines(), 
                    wall_idx, // inset_idx
                    false // is_outer_wall
                );
                
                // 转换为 VariableWidthLines
                for (const Polygon& poly : wall_outline) {
                    ExtrusionLine line;
                    for (const Point2LL& point : poly) {
                        line.emplace_back(point, line_width_x, 1);
                    }
                    if (!line.empty()) {
                        part->wall_toolpaths.back().toolpaths.push_back(line);
                    }
                }
            }
            current_outline = wall_outline;
        } else {
            break; // 无法生成更多墙体
        }
    }
    
    // 更新内部轮廓用于填充
    part->inner_area = current_outline;
    
    spdlog::debug("【简单偏移】墙体生成完成，生成了 {} 个墙体层", part->wall_toolpaths.size());
}
```

### 3. 墙体偏移修复

**问题**: 简单偏移算法缺少必要的偏移处理。

**解决方案**: 确保外墙正确偏移半个线宽。

```cpp
void WallsComputation::generateWallsWithSimpleOffset(SliceLayerPart* part, SectionType section_type)
{
    // 关键修复：确保外墙向内偏移半个线宽
    coord_t wall_0_inset = line_width_0 / 2;
    Shape wall_0_outline = part->outline.offset(-wall_0_inset);
    
    // 验证偏移结果
    if (wall_0_outline.empty()) {
        spdlog::warn("【简单偏移】外墙偏移后为空，原始轮廓可能过小");
        return;
    }
    
    spdlog::debug("【简单偏移】外墙偏移: {}μm，原始多边形: {}，偏移后: {}", 
                 wall_0_inset, part->outline.size(), wall_0_outline.size());
}
```

## 技术特点

### 1. 完全绕过 BeadingStrategy

- **直接偏移**: 使用简单的多边形偏移算法
- **固定线宽**: 使用用户设定的固定线宽值
- **高性能**: 避免复杂的线宽计算，提高切片速度

### 2. 兼容性保证

- **向后兼容**: 默认启用 BeadingStrategy，不影响现有行为
- **数据结构兼容**: 生成的数据结构与原系统兼容
- **参数兼容**: 支持所有现有的墙体相关参数

### 3. 调试支持

- **详细日志**: 提供完整的调试信息
- **性能监控**: 监控算法执行时间
- **质量验证**: 验证生成的墙体质量

## 相关参数

### 核心参数

1. **beading_strategy_enable**
   - 类型: 布尔值
   - 默认值: true
   - 描述: 启用高级 BeadingStrategy 系统

### 影响的参数

1. **wall_line_width_0**: 外墙线宽
2. **wall_line_width_x**: 内墙线宽
3. **wall_line_count**: 墙体数量
4. **wall_thickness**: 墙体总厚度

## 应用场景

### 1. 简单几何模型

对于简单的几何模型，简单偏移算法可能更合适。

### 2. 高速切片

需要快速切片时，可以禁用复杂的 BeadingStrategy。

### 3. 调试和测试

在调试墙体生成问题时，简单算法更容易理解。

### 4. 特殊材料

某些特殊材料可能更适合固定线宽的简单算法。

## 性能对比

### BeadingStrategy 系统
- **优点**: 精确的线宽控制，更好的打印质量
- **缺点**: 计算复杂，切片时间长

### 简单偏移算法
- **优点**: 计算简单，切片速度快
- **缺点**: 线宽控制不够精确

## 调试和监控

### 日志输出

```cpp
spdlog::debug("【BeadingStrategy】系统状态: {}", 
             beading_strategy_enable ? "启用" : "禁用");
spdlog::debug("【简单偏移】墙体生成完成，用时: {}ms", duration.count());
```

### 质量检查

- **墙体数量**: 验证生成的墙体数量是否正确
- **线宽一致性**: 检查线宽是否符合设定值
- **几何完整性**: 验证生成的几何体是否有效

## 未来改进方向

### 1. 混合策略

结合两种算法的优点，在不同场景下自动选择。

### 2. 智能切换

根据模型复杂度自动选择合适的算法。

### 3. 性能优化

进一步优化简单偏移算法的性能。
