# CuraEngine BeadingStrategy 系统深度分析

## 概述

BeadingStrategy（珠状策略）是 CuraEngine 中用于优化壁线宽度分布的核心算法系统。该系统的主要目标是在给定的模型厚度下，智能地确定壁线数量和每条壁线的宽度，以实现最佳的打印质量和材料利用率。

## 核心概念

### 1. Beading 结构

**文件位置**: `CuraEngine/include/BeadingStrategy/BeadingStrategy.h`

```cpp
struct Beading
{
    coord_t total_thickness;                    // 总厚度
    std::vector<coord_t> bead_widths;          // 每条珠线的宽度（从外到内）
    std::vector<coord_t> toolpath_locations;   // 每条珠线的工具路径位置（距离轮廓的距离）
    coord_t left_over;                         // 未被任何珠线覆盖的剩余距离（间隙区域）
};
```

### 2. BeadingStrategy 基类

这是一个抽象基类，定义了所有珠状策略的通用接口：

```cpp
class BeadingStrategy
{
protected:
    coord_t optimal_width_;                    // 最优珠线宽度
    Ratio wall_split_middle_threshold_;        // 中间壁分割阈值
    Ratio wall_add_middle_threshold_;          // 中间壁添加阈值
    coord_t default_transition_length_;        // 默认过渡长度
    AngleRadians transitioning_angle_;         // 过渡角度

public:
    // 核心计算函数
    virtual Beading compute(coord_t thickness, coord_t bead_count) const = 0;
    virtual coord_t getOptimalBeadCount(coord_t thickness) const = 0;
    virtual coord_t getOptimalThickness(coord_t bead_count) const;
    virtual coord_t getTransitionThickness(coord_t lower_bead_count) const;
    virtual coord_t getTransitioningLength(coord_t lower_bead_count) const;
    virtual double getTransitionAnchorPos(coord_t lower_bead_count) const;
    virtual std::vector<coord_t> getNonlinearThicknesses(coord_t lower_bead_count) const;
};
```

## 策略层次结构

### 1. BeadingStrategyFactory 工厂类

**文件位置**: `CuraEngine/src/BeadingStrategy/BeadingStrategyFactory.cpp`

工厂类负责创建复合的珠状策略链：

```cpp
BeadingStrategyPtr BeadingStrategyFactory::makeStrategy(
    const coord_t preferred_bead_width_outer,      // 外壁首选宽度
    const coord_t preferred_bead_width_inner,      // 内壁首选宽度
    const coord_t preferred_transition_length,     // 首选过渡长度
    const double transitioning_angle,              // 过渡角度
    const bool print_thin_walls,                   // 是否打印薄壁
    const coord_t min_bead_width,                  // 最小珠线宽度
    const coord_t min_feature_size,                // 最小特征尺寸
    const Ratio wall_split_middle_threshold,       // 壁分割阈值
    const Ratio wall_add_middle_threshold,         // 壁添加阈值
    const coord_t max_bead_count,                  // 最大珠线数量
    const coord_t outer_wall_offset,               // 外壁偏移
    const int inward_distributed_center_wall_count, // 向内分布的中心壁数量
    const Ratio minimum_variable_line_ratio)       // 最小可变线宽比例
{
    // 1. 基础策略：DistributedBeadingStrategy
    BeadingStrategyPtr ret = make_unique<DistributedBeadingStrategy>(
        preferred_bead_width_inner,
        preferred_transition_length,
        transitioning_angle,
        wall_split_middle_threshold,
        wall_add_middle_threshold,
        inward_distributed_center_wall_count);
    
    // 2. 重分布策略：RedistributeBeadingStrategy
    ret = make_unique<RedistributeBeadingStrategy>(
        preferred_bead_width_outer, 
        minimum_variable_line_ratio, 
        std::move(ret));
    
    // 3. 薄壁策略：WideningBeadingStrategy（可选）
    if (print_thin_walls) {
        ret = make_unique<WideningBeadingStrategy>(
            std::move(ret), 
            min_feature_size, 
            min_bead_width);
    }
    
    // 4. 外壁偏移策略：OuterWallInsetBeadingStrategy（可选）
    if (outer_wall_offset > 0) {
        ret = make_unique<OuterWallInsetBeadingStrategy>(
            outer_wall_offset, 
            std::move(ret));
    }
    
    // 5. 限制策略：LimitedBeadingStrategy（最后应用）
    ret = make_unique<LimitedBeadingStrategy>(
        max_bead_count, 
        std::move(ret));
    
    return ret;
}
```

### 2. 策略组合模式

BeadingStrategy 使用装饰器模式，形成策略链：

```
LimitedBeadingStrategy
  └── OuterWallInsetBeadingStrategy (可选)
      └── WideningBeadingStrategy (可选)
          └── RedistributeBeadingStrategy
              └── DistributedBeadingStrategy (基础策略)
```

## 核心策略详解

### 1. DistributedBeadingStrategy（分布式珠状策略）

**文件位置**: `CuraEngine/include/BeadingStrategy/DistributedBeadingStrategy.h`

这是基础策略，负责在给定厚度下均匀分布珠线：

```cpp
class DistributedBeadingStrategy : public BeadingStrategy
{
protected:
    double one_over_distribution_radius_squared_; // (1 / distribution_radius)^2

public:
    DistributedBeadingStrategy(
        const coord_t optimal_width,
        const coord_t default_transition_length,
        const AngleRadians transitioning_angle,
        const Ratio wall_split_middle_threshold,
        const Ratio wall_add_middle_threshold,
        const int distribution_radius);

    Beading compute(coord_t thickness, coord_t bead_count) const override;
    coord_t getOptimalBeadCount(coord_t thickness) const override;
};
```

**核心算法**：
1. 根据厚度计算最优珠线数量
2. 在分布半径内均匀分配宽度偏差
3. 确保珠线宽度在合理范围内

### 2. RedistributeBeadingStrategy（重分布珠状策略）

**文件位置**: `CuraEngine/include/BeadingStrategy/RedistributeBeadingStrategy.h`

负责重新分布珠线宽度，特别是外壁的处理：

```cpp
class RedistributeBeadingStrategy : public BeadingStrategy
{
protected:
    BeadingStrategyPtr parent_;                    // 父策略
    coord_t optimal_width_outer_;                  // 外壁最优宽度
    Ratio minimum_variable_line_ratio_;            // 最小可变线宽比例

public:
    RedistributeBeadingStrategy(
        const coord_t optimal_width_outer,
        const Ratio minimum_variable_line_ratio,
        BeadingStrategyPtr parent);

    Beading compute(coord_t thickness, coord_t bead_count) const override;
    coord_t getOptimalBeadCount(coord_t thickness) const override;
};
```

**核心功能**：
1. 优化外壁宽度以获得更好的表面质量
2. 重新分布内壁宽度以补偿外壁调整
3. 确保线宽变化在可接受范围内

### 3. WideningBeadingStrategy（加宽珠状策略）

**文件位置**: `CuraEngine/include/BeadingStrategy/WideningBeadingStrategy.h`

处理薄壁特征，确保小特征能够被正确打印：

```cpp
class WideningBeadingStrategy : public BeadingStrategy
{
protected:
    BeadingStrategyPtr parent_;                    // 父策略
    coord_t min_input_width_;                      // 最小输入宽度
    coord_t min_output_width_;                     // 最小输出宽度

public:
    WideningBeadingStrategy(
        BeadingStrategyPtr parent,
        const coord_t min_input_width,
        const coord_t min_output_width);

    Beading compute(coord_t thickness, coord_t bead_count) const override;
    coord_t getOptimalBeadCount(coord_t thickness) const override;
};
```

**核心功能**：
1. 检测薄壁特征（厚度小于最小输入宽度）
2. 将薄壁加宽到可打印的最小宽度
3. 保持薄壁的几何形状和连续性

### 4. OuterWallInsetBeadingStrategy（外壁内缩珠状策略）

**文件位置**: `CuraEngine/include/BeadingStrategy/OuterWallInsetBeadingStrategy.h`

为外壁添加偏移，改善表面质量：

```cpp
class OuterWallInsetBeadingStrategy : public BeadingStrategy
{
protected:
    BeadingStrategyPtr parent_;                    // 父策略
    coord_t outer_wall_offset_;                    // 外壁偏移量

public:
    OuterWallInsetBeadingStrategy(
        const coord_t outer_wall_offset,
        BeadingStrategyPtr parent);

    Beading compute(coord_t thickness, coord_t bead_count) const override;
    coord_t getOptimalBeadCount(coord_t thickness) const override;
};
```

**核心功能**：
1. 将外壁向内偏移指定距离
2. 调整内壁位置以填充剩余空间
3. 改善外表面质量和尺寸精度

### 5. LimitedBeadingStrategy（限制珠状策略）

**文件位置**: `CuraEngine/include/BeadingStrategy/LimitedBeadingStrategy.h`

限制最大珠线数量，添加零宽度标记壁：

```cpp
class LimitedBeadingStrategy : public BeadingStrategy
{
protected:
    BeadingStrategyPtr parent_;                    // 父策略
    coord_t max_bead_count_;                       // 最大珠线数量

public:
    LimitedBeadingStrategy(
        const coord_t max_bead_count,
        BeadingStrategyPtr parent);

    Beading compute(coord_t thickness, coord_t bead_count) const override;
    coord_t getOptimalBeadCount(coord_t thickness) const override;
};
```

**核心功能**：
1. 限制珠线数量不超过最大值
2. 为超出部分添加零宽度标记壁
3. 确保其他策略不会修改标记壁

## 算法核心逻辑

### 1. 最优珠线数量计算

```cpp
coord_t getOptimalBeadCount(coord_t thickness) const
{
    // 基本计算：厚度除以最优宽度
    coord_t basic_count = thickness / optimal_width_;
    
    // 考虑分割和添加阈值
    coord_t split_threshold = optimal_width_ * wall_split_middle_threshold_;
    coord_t add_threshold = optimal_width_ * wall_add_middle_threshold_;
    
    // 根据剩余厚度决定是否调整珠线数量
    coord_t remainder = thickness % optimal_width_;
    
    if (remainder > add_threshold) {
        return basic_count + 1;
    } else if (remainder < split_threshold && basic_count > 1) {
        return basic_count - 1;
    }
    
    return basic_count;
}
```

### 2. 珠线宽度分布计算

```cpp
Beading compute(coord_t thickness, coord_t bead_count) const
{
    Beading result;
    result.total_thickness = thickness;
    
    if (bead_count == 0) {
        result.left_over = thickness;
        return result;
    }
    
    // 计算平均宽度
    coord_t average_width = thickness / bead_count;
    
    // 分布宽度偏差
    coord_t width_deviation = average_width - optimal_width_;
    
    // 在分布半径内均匀分配偏差
    for (int i = 0; i < bead_count; ++i) {
        coord_t distance_from_center = std::abs(i - bead_count / 2);
        double distribution_factor = std::exp(-distance_from_center * distance_from_center * 
                                            one_over_distribution_radius_squared_);
        
        coord_t bead_width = optimal_width_ + width_deviation * distribution_factor;
        result.bead_widths.push_back(bead_width);
    }
    
    // 计算工具路径位置
    coord_t current_position = 0;
    for (coord_t width : result.bead_widths) {
        current_position += width / 2;
        result.toolpath_locations.push_back(current_position);
        current_position += width / 2;
    }
    
    // 计算剩余空间
    coord_t total_used = std::accumulate(result.bead_widths.begin(), 
                                       result.bead_widths.end(), 0);
    result.left_over = thickness - total_used;
    
    return result;
}
```

### 3. 过渡处理

```cpp
coord_t getTransitionThickness(coord_t lower_bead_count) const
{
    // 计算从 lower_bead_count 到 lower_bead_count + 1 的过渡厚度
    coord_t optimal_thickness_lower = getOptimalThickness(lower_bead_count);
    coord_t optimal_thickness_upper = getOptimalThickness(lower_bead_count + 1);
    
    // 使用阈值确定过渡点
    coord_t threshold_width = optimal_width_ * wall_add_middle_threshold_;
    
    return optimal_thickness_lower + threshold_width;
}

coord_t getTransitioningLength(coord_t lower_bead_count) const
{
    // 根据角度和默认长度计算过渡长度
    double angle_factor = std::sin(transitioning_angle_);
    return default_transition_length_ * angle_factor;
}
```

## 参数配置和调优

### 1. 关键参数

1. **optimal_width_**: 最优珠线宽度（通常等于喷嘴直径）
2. **wall_split_middle_threshold_**: 中间壁分割阈值（默认0.5）
3. **wall_add_middle_threshold_**: 中间壁添加阈值（默认0.5）
4. **default_transition_length_**: 默认过渡长度（默认0.4mm）
5. **transitioning_angle_**: 过渡角度（默认π/4）
6. **distribution_radius**: 分布半径（默认2）

### 2. 调优策略

1. **表面质量优先**: 增大外壁宽度，减小内壁宽度变化
2. **强度优先**: 均匀分布所有壁线宽度
3. **速度优先**: 减少珠线数量，增大线宽
4. **精度优先**: 减小线宽变化，增加过渡长度

### 3. 特殊情况处理

1. **薄壁**: 启用 WideningBeadingStrategy
2. **厚壁**: 限制最大珠线数量
3. **变厚度**: 增加过渡长度
4. **小特征**: 调整最小特征尺寸

## 性能优化

### 1. 计算优化

- 预计算常用的数学函数值
- 缓存重复计算的结果
- 使用整数运算替代浮点运算

### 2. 内存优化

- 使用智能指针管理策略链
- 避免不必要的数据拷贝
- 复用临时对象

### 3. 算法优化

- 早期退出条件检查
- 分层计算复杂度
- 并行处理独立计算

## 应用场景

### 1. 不同材料的优化

- **PLA**: 标准参数配置
- **ABS**: 增大过渡长度
- **PETG**: 调整分割阈值
- **TPU**: 特殊的薄壁处理

### 2. 不同几何形状

- **圆形**: 均匀分布策略
- **方形**: 角落特殊处理
- **复杂形状**: 自适应过渡
- **薄壁**: 加宽策略

### 3. 质量要求

- **原型**: 快速打印配置
- **功能件**: 强度优化配置
- **展示件**: 表面质量优化
- **精密件**: 尺寸精度优化

## 详细实现分析

### 1. DistributedBeadingStrategy 实现

**文件位置**: `CuraEngine/src/BeadingStrategy/DistributedBeadingStrategy.cpp`

```cpp
Beading DistributedBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    Beading ret;
    ret.total_thickness = thickness;

    if (bead_count == 0) {
        ret.left_over = thickness;
        return ret;
    }

    // 计算理想的珠线宽度
    const coord_t optimal_thickness = bead_count * optimal_width_;
    const coord_t thickness_diff = thickness - optimal_thickness;

    // 分布宽度调整
    ret.bead_widths.resize(bead_count);
    ret.toolpath_locations.resize(bead_count);

    coord_t current_x = 0;
    for (coord_t bead_idx = 0; bead_idx < bead_count; bead_idx++) {
        // 计算距离中心的距离
        const coord_t center_bead_idx = (bead_count - 1) / 2.0;
        const double distance_from_center = bead_idx - center_bead_idx;

        // 应用分布函数
        const double distribution_factor = std::exp(-distance_from_center * distance_from_center *
                                                   one_over_distribution_radius_squared_);

        // 计算调整后的宽度
        const coord_t width_adjustment = thickness_diff * distribution_factor / bead_count;
        ret.bead_widths[bead_idx] = optimal_width_ + width_adjustment;

        // 计算工具路径位置
        current_x += ret.bead_widths[bead_idx] / 2;
        ret.toolpath_locations[bead_idx] = current_x;
        current_x += ret.bead_widths[bead_idx] / 2;
    }

    // 计算剩余厚度
    const coord_t total_bead_width = std::accumulate(ret.bead_widths.begin(),
                                                   ret.bead_widths.end(), 0);
    ret.left_over = thickness - total_bead_width;

    return ret;
}

coord_t DistributedBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    if (thickness < optimal_width_ / 2) {
        return 0;
    }

    // 基础计算
    coord_t basic_bead_count = std::round(static_cast<double>(thickness) / optimal_width_);

    // 考虑分割和添加阈值
    const coord_t remainder = thickness - basic_bead_count * optimal_width_;
    const coord_t split_threshold = optimal_width_ * wall_split_middle_threshold_;
    const coord_t add_threshold = optimal_width_ * wall_add_middle_threshold_;

    if (remainder > add_threshold && basic_bead_count > 0) {
        return basic_bead_count + 1;
    } else if (remainder < -split_threshold && basic_bead_count > 1) {
        return basic_bead_count - 1;
    }

    return std::max(static_cast<coord_t>(1), basic_bead_count);
}
```

### 2. RedistributeBeadingStrategy 实现

**文件位置**: `CuraEngine/src/BeadingStrategy/RedistributeBeadingStrategy.cpp`

```cpp
Beading RedistributeBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    // 首先获取父策略的结果
    Beading ret = parent_->compute(thickness, bead_count);

    if (ret.bead_widths.empty()) {
        return ret;
    }

    // 重新分布外壁宽度
    if (ret.bead_widths.size() >= 1) {
        // 调整最外层壁线宽度
        const coord_t outer_width_diff = optimal_width_outer_ - ret.bead_widths[0];
        ret.bead_widths[0] = optimal_width_outer_;

        // 将差值分布到内壁
        if (ret.bead_widths.size() > 1) {
            const coord_t inner_adjustment = outer_width_diff / (ret.bead_widths.size() - 1);
            for (size_t i = 1; i < ret.bead_widths.size(); ++i) {
                ret.bead_widths[i] -= inner_adjustment;

                // 确保内壁宽度不会过小
                const coord_t min_inner_width = optimal_width_outer_ * minimum_variable_line_ratio_;
                ret.bead_widths[i] = std::max(ret.bead_widths[i], min_inner_width);
            }
        }
    }

    // 重新计算工具路径位置
    coord_t current_x = 0;
    for (size_t i = 0; i < ret.bead_widths.size(); ++i) {
        current_x += ret.bead_widths[i] / 2;
        ret.toolpath_locations[i] = current_x;
        current_x += ret.bead_widths[i] / 2;
    }

    // 重新计算剩余厚度
    const coord_t total_bead_width = std::accumulate(ret.bead_widths.begin(),
                                                   ret.bead_widths.end(), 0);
    ret.left_over = thickness - total_bead_width;

    return ret;
}
```

### 3. WideningBeadingStrategy 实现

**文件位置**: `CuraEngine/src/BeadingStrategy/WideningBeadingStrategy.cpp`

```cpp
Beading WideningBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    // 检查是否需要加宽处理
    if (thickness < min_input_width_) {
        Beading ret;
        ret.total_thickness = thickness;

        if (thickness >= min_output_width_) {
            // 创建单个加宽的珠线
            ret.bead_widths.push_back(min_output_width_);
            ret.toolpath_locations.push_back(min_output_width_ / 2);
            ret.left_over = thickness - min_output_width_;
        } else {
            // 厚度太小，无法打印
            ret.left_over = thickness;
        }

        return ret;
    }

    // 厚度足够，使用父策略
    return parent_->compute(thickness, bead_count);
}

coord_t WideningBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    if (thickness < min_input_width_) {
        return (thickness >= min_output_width_) ? 1 : 0;
    }

    return parent_->getOptimalBeadCount(thickness);
}
```

### 4. OuterWallInsetBeadingStrategy 实现

```cpp
Beading OuterWallInsetBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    // 调整厚度以考虑外壁偏移
    const coord_t adjusted_thickness = thickness - outer_wall_offset_;

    if (adjusted_thickness <= 0) {
        Beading ret;
        ret.total_thickness = thickness;
        ret.left_over = thickness;
        return ret;
    }

    // 使用调整后的厚度计算
    Beading ret = parent_->compute(adjusted_thickness, bead_count);
    ret.total_thickness = thickness;

    // 调整工具路径位置以考虑偏移
    for (coord_t& location : ret.toolpath_locations) {
        location += outer_wall_offset_;
    }

    return ret;
}
```

### 5. LimitedBeadingStrategy 实现

```cpp
Beading LimitedBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    // 限制珠线数量
    const coord_t limited_bead_count = std::min(bead_count, max_bead_count_);

    Beading ret = parent_->compute(thickness, limited_bead_count);

    // 如果超出限制，添加零宽度标记壁
    if (bead_count > max_bead_count_) {
        const coord_t excess_beads = bead_count - max_bead_count_;

        for (coord_t i = 0; i < excess_beads; ++i) {
            ret.bead_widths.push_back(0);  // 零宽度标记
            ret.toolpath_locations.push_back(ret.toolpath_locations.back());
        }
    }

    return ret;
}

coord_t LimitedBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    const coord_t parent_count = parent_->getOptimalBeadCount(thickness);
    return std::min(parent_count, max_bead_count_);
}
```

## 高级算法技术

### 1. 过渡处理算法

```cpp
coord_t BeadingStrategy::getTransitionThickness(coord_t lower_bead_count) const
{
    const coord_t lower_optimal = getOptimalThickness(lower_bead_count);
    const coord_t upper_optimal = getOptimalThickness(lower_bead_count + 1);

    // 使用阈值确定过渡点
    const coord_t threshold_width = optimal_width_ * wall_add_middle_threshold_;
    const coord_t transition_point = lower_optimal + threshold_width;

    return std::min(transition_point, (lower_optimal + upper_optimal) / 2);
}

coord_t BeadingStrategy::getTransitioningLength(coord_t lower_bead_count) const
{
    // 根据角度和默认长度计算过渡长度
    const double angle_factor = std::sin(transitioning_angle_);
    const coord_t base_length = default_transition_length_;

    // 考虑珠线数量的影响
    const double bead_count_factor = 1.0 + 0.1 * lower_bead_count;

    return base_length * angle_factor * bead_count_factor;
}

double BeadingStrategy::getTransitionAnchorPos(coord_t lower_bead_count) const
{
    // 过渡锚点位置（0.0 = 过渡开始，1.0 = 过渡结束）
    // 通常设置为 0.4，使过渡更平滑
    return 0.4;
}
```

### 2. 非线性厚度计算

```cpp
std::vector<coord_t> BeadingStrategy::getNonlinearThicknesses(coord_t lower_bead_count) const
{
    std::vector<coord_t> result;

    const coord_t lower_optimal = getOptimalThickness(lower_bead_count);
    const coord_t upper_optimal = getOptimalThickness(lower_bead_count + 1);

    // 在过渡区域内添加关键点
    const coord_t transition_start = getTransitionThickness(lower_bead_count);
    const coord_t transition_length = getTransitioningLength(lower_bead_count);
    const coord_t transition_end = transition_start + transition_length;

    // 添加过渡区域的关键厚度点
    if (transition_start < upper_optimal) {
        result.push_back(transition_start);

        // 在过渡区域内添加中间点
        const coord_t quarter_point = transition_start + transition_length * 0.25;
        const coord_t half_point = transition_start + transition_length * 0.5;
        const coord_t three_quarter_point = transition_start + transition_length * 0.75;

        if (quarter_point < upper_optimal) result.push_back(quarter_point);
        if (half_point < upper_optimal) result.push_back(half_point);
        if (three_quarter_point < upper_optimal) result.push_back(three_quarter_point);
        if (transition_end < upper_optimal) result.push_back(transition_end);
    }

    return result;
}
```

### 3. 自适应宽度分布

```cpp
class AdaptiveDistributedBeadingStrategy : public DistributedBeadingStrategy
{
public:
    Beading compute(coord_t thickness, coord_t bead_count) const override
    {
        Beading base_result = DistributedBeadingStrategy::compute(thickness, bead_count);

        if (base_result.bead_widths.empty()) {
            return base_result;
        }

        // 自适应调整：根据位置调整宽度分布
        for (size_t i = 0; i < base_result.bead_widths.size(); ++i) {
            double position_factor = static_cast<double>(i) / (base_result.bead_widths.size() - 1);

            // 外壁（i=0）保持稳定，内壁可以有更大变化
            double variability = (i == 0) ? 0.1 : 0.3;

            // 根据厚度偏差调整
            double thickness_ratio = static_cast<double>(thickness) / (bead_count * optimal_width_);
            double adjustment_factor = (thickness_ratio - 1.0) * variability;

            base_result.bead_widths[i] *= (1.0 + adjustment_factor);

            // 确保宽度在合理范围内
            base_result.bead_widths[i] = std::clamp(base_result.bead_widths[i],
                                                   optimal_width_ * 0.5,
                                                   optimal_width_ * 1.5);
        }

        // 重新计算工具路径位置
        coord_t current_x = 0;
        for (size_t i = 0; i < base_result.bead_widths.size(); ++i) {
            current_x += base_result.bead_widths[i] / 2;
            base_result.toolpath_locations[i] = current_x;
            current_x += base_result.bead_widths[i] / 2;
        }

        return base_result;
    }
};
```

## 质量控制和验证

### 1. 宽度一致性检查

```cpp
bool validateBeadingConsistency(const Beading& beading, coord_t tolerance = 50) // 50微米容差
{
    if (beading.bead_widths.empty()) {
        return true;
    }

    // 检查宽度变化是否在容差范围内
    for (size_t i = 1; i < beading.bead_widths.size(); ++i) {
        coord_t width_diff = std::abs(beading.bead_widths[i] - beading.bead_widths[i-1]);
        if (width_diff > tolerance) {
            spdlog::warn("Large width variation detected: {} -> {} (diff: {})",
                        beading.bead_widths[i-1], beading.bead_widths[i], width_diff);
            return false;
        }
    }

    return true;
}
```

### 2. 厚度覆盖验证

```cpp
bool validateThicknessCoverage(const Beading& beading, coord_t expected_thickness,
                              coord_t tolerance = 25) // 25微米容差
{
    coord_t total_width = std::accumulate(beading.bead_widths.begin(),
                                        beading.bead_widths.end(), 0);
    coord_t covered_thickness = total_width + beading.left_over;

    coord_t thickness_error = std::abs(covered_thickness - expected_thickness);

    if (thickness_error > tolerance) {
        spdlog::warn("Thickness coverage error: expected {}, got {} (error: {})",
                    expected_thickness, covered_thickness, thickness_error);
        return false;
    }

    return true;
}
```

### 3. 工具路径位置验证

```cpp
bool validateToolpathPositions(const Beading& beading)
{
    if (beading.bead_widths.size() != beading.toolpath_locations.size()) {
        spdlog::error("Mismatch between bead count and toolpath location count");
        return false;
    }

    coord_t expected_position = 0;
    for (size_t i = 0; i < beading.bead_widths.size(); ++i) {
        expected_position += beading.bead_widths[i] / 2;

        coord_t position_error = std::abs(beading.toolpath_locations[i] - expected_position);
        if (position_error > 10) { // 10微米容差
            spdlog::warn("Toolpath position error at index {}: expected {}, got {}",
                        i, expected_position, beading.toolpath_locations[i]);
            return false;
        }

        expected_position += beading.bead_widths[i] / 2;
    }

    return true;
}
```

## 性能优化实现

### 1. 缓存机制

```cpp
class CachedBeadingStrategy : public BeadingStrategy
{
private:
    mutable std::unordered_map<std::pair<coord_t, coord_t>, Beading> cache_;
    BeadingStrategyPtr parent_;

public:
    Beading compute(coord_t thickness, coord_t bead_count) const override
    {
        auto key = std::make_pair(thickness, bead_count);
        auto it = cache_.find(key);

        if (it != cache_.end()) {
            return it->second;
        }

        Beading result = parent_->compute(thickness, bead_count);
        cache_[key] = result;

        return result;
    }
};
```

### 2. 并行计算

```cpp
std::vector<Beading> computeBeadingsParallel(const BeadingStrategy& strategy,
                                           const std::vector<std::pair<coord_t, coord_t>>& inputs)
{
    std::vector<Beading> results(inputs.size());

    std::for_each(std::execution::par_unseq,
                  inputs.begin(), inputs.end(),
                  [&](const auto& input) {
                      size_t index = &input - &inputs[0];
                      results[index] = strategy.compute(input.first, input.second);
                  });

    return results;
}
```

BeadingStrategy 系统通过多层策略组合和智能算法，实现了对壁线宽度的精确控制，是 CuraEngine 实现高质量打印的关键技术之一。通过深入理解这些实现细节，开发者可以更好地定制和优化打印策略。
