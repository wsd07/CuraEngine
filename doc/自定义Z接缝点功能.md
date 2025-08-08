# 自定义Z接缝点功能

## 概述

自定义Z接缝点功能允许用户通过指定3D坐标点来精确控制每层外轮廓的接缝位置。系统会根据当前层高度自动进行线性插值计算，实现更精细的接缝控制和更好的打印质量。

## 核心功能

### 1. 基础接缝点功能

**参数**: `draw_z_seam_enable`, `draw_z_seam_points`

#### 实现逻辑

**文件位置**: `CuraEngine/src/utils/ZSeamConfig.cpp`

```cpp
std::optional<Point2LL> ZSeamConfig::getInterpolatedSeamPosition() const
{
    if (!draw_z_seam_enable_ || draw_z_seam_points_.empty()) {
        return std::nullopt;
    }

    // 线性插值计算当前层的接缝位置
    for (size_t i = 0; i < draw_z_seam_points_.size() - 1; i++) {
        const auto& p1 = draw_z_seam_points_[i];
        const auto& p2 = draw_z_seam_points_[i + 1];
        
        if (current_layer_z_ >= p1.z && current_layer_z_ <= p2.z) {
            double ratio = (current_layer_z_ - p1.z) / (p2.z - p1.z);
            coord_t x = p1.x + ratio * (p2.x - p1.x);
            coord_t y = p1.y + ratio * (p2.y - p1.y);
            return Point2LL(x, y);
        }
    }
    
    return std::nullopt;
}
```

### 2. 多边形预处理插值

**问题**: 原始多边形可能没有足够接近接缝点的顶点。

**解决方案**: 在多边形预处理阶段插入插值点。

**文件位置**: `CuraEngine/src/sliceDataStorage.cpp`

```cpp
void SliceDataStorage::interpolateZSeamPoints()
{
    if (!mesh_group_settings.get<bool>("draw_z_seam_enable")) {
        return;
    }

    for (size_t layer_idx = 0; layer_idx < layers.size(); layer_idx++) {
        SliceLayer& layer = layers[layer_idx];
        coord_t layer_z = layer.printZ;
        
        // 获取当前层的插值接缝位置
        ZSeamConfig seam_config(/* ... */);
        seam_config.current_layer_z_ = layer_z;
        auto interpolated_pos = seam_config.getInterpolatedSeamPosition();
        
        if (interpolated_pos.has_value()) {
            Point2LL target_point = interpolated_pos.value();
            
            // 对每个part的每个多边形进行插值点插入
            for (SliceLayerPart& part : layer.parts) {
                for (Polygon& polygon : part.outline) {
                    insertInterpolationPoint(polygon, target_point);
                }
            }
        }
    }
}
```

### 3. 螺旋模式支持

**问题**: 螺旋模式下接缝点功能失效。

**解决方案**: 在螺旋模式处理中添加接缝点支持。

**文件位置**: `CuraEngine/src/WallsComputation.cpp`

```cpp
void WallsComputation::generateSpiralInsets(SliceLayerPart* part, coord_t line_width_0, coord_t wall_0_inset, bool recompute_outline_based_on_outer_wall)
{
    // 检查是否启用了自定义Z接缝点
    const bool draw_z_seam_enable = settings_.get<bool>("draw_z_seam_enable");
    if (draw_z_seam_enable) {
        // 对螺旋轮廓进行插值点插入
        ZSeamConfig seam_config(/* ... */);
        seam_config.current_layer_z_ = layer_z;
        auto interpolated_pos = seam_config.getInterpolatedSeamPosition();
        
        if (interpolated_pos.has_value()) {
            Point2LL target_point = interpolated_pos.value();
            for (Polygon& polygon : part->spiral_wall) {
                insertInterpolationPoint(polygon, target_point);
            }
        }
    }
}
```

### 4. 可变层厚支持

**问题**: 可变层厚模式下Z坐标计算错误。

**解决方案**: 使用实际层Z坐标而不是计算值。

```cpp
// 错误的计算方式
coord_t layer_z = layer_idx * layer_height;

// 正确的计算方式
coord_t layer_z = layers[layer_idx].printZ;
```

### 5. Raft结构影响修复

**问题**: 有Raft结构时，层Z坐标包含Raft厚度，导致接缝点计算错误。

**解决方案**: 计算模型实际高度，排除Raft影响。

```cpp
// 计算模型实际高度（排除raft影响）
coord_t model_z = layer_z;
if (raft_layers > 0) {
    model_z = layer_z - raft_total_thickness;
}

// 使用模型高度进行接缝点插值
seam_config.current_layer_z_ = model_z;
```

## 高级功能

### 1. 手绘接缝点墙线优先打印

**参数**: `z_seam_part_print_first`

**功能**: 让包含接缝点的墙线优先打印。

**实现**: 在路径优化阶段识别包含接缝点的墙线，调整打印顺序。

### 2. 插值点精度控制

**参数**: 插值距离阈值（内部参数）

**功能**: 控制插值点插入的精度，平衡质量和性能。

## 相关参数

### 核心参数

1. **draw_z_seam_enable**
   - 类型: 布尔值
   - 描述: 启用自定义Z接缝点功能

2. **draw_z_seam_points**
   - 类型: 3D坐标点列表
   - 描述: 用户定义的接缝点坐标

3. **z_seam_part_print_first**
   - 类型: 布尔值
   - 描述: 包含接缝点的墙线优先打印

### 辅助参数

1. **z_seam_type**
   - 影响: 与自定义接缝点的优先级关系

2. **z_seam_position**
   - 影响: 回退策略的选择

## 应用场景

### 1. 精密打印

通过精确控制接缝位置，提高打印件的外观质量。

### 2. 功能性部件

将接缝放在不影响功能的位置，如背面或内侧。

### 3. 艺术品打印

通过接缝位置控制创造特殊的视觉效果。

### 4. 螺旋模式增强

在螺旋模式下实现精确的接缝控制。

## 技术挑战与解决方案

### 1. 插值精度

**挑战**: 如何在保证精度的同时控制性能开销。

**解决方案**: 使用距离阈值控制插值点密度。

### 2. 多边形完整性

**挑战**: 插入点后如何保证多边形的几何完整性。

**解决方案**: 验证插入点的合理性，避免自相交。

### 3. 兼容性

**挑战**: 如何与现有的接缝策略兼容。

**解决方案**: 实现优先级机制，自定义接缝点优先级最高。

### 4. 性能优化

**挑战**: 大量插值计算对性能的影响。

**解决方案**: 缓存计算结果，避免重复计算。

## 调试和监控

### 日志输出

```cpp
spdlog::info("【自定义接缝】层{}，插值位置: ({:.2f}, {:.2f})", 
            layer_idx, INT2MM(target_point.X), INT2MM(target_point.Y));
spdlog::info("【自定义接缝】在索引{}插入新点: ({:.2f}, {:.2f})", 
            insert_idx, INT2MM(new_point.X), INT2MM(new_point.Y));
```

### 质量验证

- **插值精度**: 验证插值点与目标点的距离
- **多边形完整性**: 检查插入点后的多边形有效性
- **接缝质量**: 评估最终接缝位置的准确性

## 未来改进方向

### 1. 智能插值

根据多边形复杂度自动调整插值策略。

### 2. 多目标优化

同时考虑接缝质量、打印速度和材料使用。

### 3. 机器学习

使用机器学习优化接缝位置选择。
