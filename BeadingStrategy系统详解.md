# CuraEngine BeadingStrategy系统详解

## 概述

BeadingStrategy（珠链策略）是CuraEngine中用于计算打印路径宽度和位置的核心系统。它解决了一个关键问题：**给定一个特定厚度的区域，如何确定打印线的数量、每条线的宽度以及它们的位置**。

## 核心概念

### 什么是Beading？
Beading是指将一个给定厚度的区域分解为多条打印线（beads）的过程。每条线有：
- **宽度（width）**：打印线的实际宽度
- **位置（location）**：打印线中心线的位置
- **数量（count）**：该区域内打印线的总数

### 核心数据结构

```cpp
struct Beading {
    coord_t total_thickness;                    // 总厚度
    std::vector<coord_t> bead_widths;          // 每条线的宽度
    std::vector<coord_t> toolpath_locations;   // 每条线的中心位置
    coord_t left_over;                         // 剩余厚度（无法打印的部分）
};
```

## 系统架构

### 基础抽象类：BeadingStrategy

所有策略的基类，定义了核心接口：

```cpp
class BeadingStrategy {
public:
    // 核心计算方法：给定厚度和线数，计算具体的线宽和位置
    virtual Beading compute(coord_t thickness, coord_t bead_count) const = 0;
    
    // 获取给定厚度的最优线数
    virtual coord_t getOptimalBeadCount(coord_t thickness) const = 0;
    
    // 获取给定线数的最优厚度
    virtual coord_t getOptimalThickness(coord_t bead_count) const;
    
    // 获取从n线到n+1线的过渡厚度
    virtual coord_t getTransitionThickness(coord_t lower_bead_count) const;
};
```

### 关键参数

- **optimal_width_**：最优线宽（通常等于喷嘴直径）
- **wall_split_middle_threshold_**：分割中间线的阈值
- **wall_add_middle_threshold_**：添加中间线的阈值
- **default_transition_length_**：默认过渡长度
- **transitioning_angle_**：过渡角度

## 具体策略实现

### 1. DistributedBeadingStrategy（分布式策略）

**作用**：基础策略，将线宽均匀分布在可用厚度上。

**核心算法**：
```cpp
// 计算每条线应该承担的额外宽度
const coord_t to_be_divided = thickness - bead_count * optimal_width_;

// 使用权重函数分配额外宽度，中间线权重最高
const auto getWeight = [middle, this](coord_t bead_idx) {
    const double dev_from_middle = bead_idx - middle;
    return std::max(0.0, 1.0 - one_over_distribution_radius_squared_ * dev_from_middle * dev_from_middle);
};
```

**效果**：
- 优先调整中间线的宽度
- 保持外线相对稳定
- 适用于多线打印的基础分配

### 2. RedistributeBeadingStrategy（重分配策略）

**作用**：确保外墙保持恒定宽度，只调整内墙。

**核心理念**：
- 外墙宽度固定，提供更好的表面质量
- 内墙承担厚度变化的调整
- 当厚度不足时，优先保证外墙质量

**应用场景**：
- 需要高表面质量的打印
- 外观重要的模型
- 减少外墙的挤出变化

### 3. WideningBeadingStrategy（加宽策略）

**作用**：处理过窄区域，将其转换为单线路径。

**核心逻辑**：
```cpp
if (thickness < optimal_width_) {
    if (thickness >= min_input_width_) {
        // 创建单线，宽度至少为min_output_width_
        ret.bead_widths.emplace_back(std::max(thickness, min_output_width_));
        ret.toolpath_locations.emplace_back(thickness / 2);
    } else {
        // 太窄，标记为left_over
        ret.left_over = thickness;
    }
}
```

**效果**：
- 防止细小特征丢失
- 将窄区域转换为可打印的单线
- 提高模型完整性

### 4. LimitedBeadingStrategy（限制策略）

**作用**：限制最大线数，并添加0宽度标记线。

**核心功能**：
```cpp
// 当达到最大线数时，添加0宽度标记线
if (bead_count % 2 == 0 && bead_count == max_bead_count_) {
    ret.toolpath_locations.insert(ret.toolpath_locations.begin() + max_bead_count_ / 2, 
                                  innermost_toolpath_location + innermost_toolpath_width / 2);
    ret.bead_widths.insert(ret.bead_widths.begin() + max_bead_count_ / 2, 0);
}
```

**0宽度标记线的作用**：
- 标记墙体区域的边界
- 为填充和表面结构提供对齐参考
- 确保其他结构正确重叠

### 5. OuterWallInsetBeadingStrategy（外墙内缩策略）

**作用**：将外墙向内偏移指定距离。

**应用场景**：
- 补偿挤出宽度
- 调整尺寸精度
- 特殊材料的补偿

## 策略组合与工厂模式

### BeadingStrategyFactory

工厂类按特定顺序组合多个策略：

```cpp
BeadingStrategyPtr makeStrategy(...) {
    // 1. 基础分布策略
    BeadingStrategyPtr ret = make_unique<DistributedBeadingStrategy>(...);
    
    // 2. 重分配策略（外墙优化）
    ret = make_unique<RedistributeBeadingStrategy>(..., std::move(ret));
    
    // 3. 窄区域处理（可选）
    if (print_thin_walls && narrow_area_merge) {
        ret = make_unique<WideningBeadingStrategy>(std::move(ret), ...);
    }
    
    // 4. 外墙偏移（可选）
    if (outer_wall_offset > 0) {
        ret = make_unique<OuterWallInsetBeadingStrategy>(..., std::move(ret));
    }
    
    // 5. 限制策略（必须最后应用）
    ret = make_unique<LimitedBeadingStrategy>(max_bead_count, std::move(ret));
    
    return ret;
}
```

### 策略链模式

每个策略都包装前一个策略，形成责任链：
- **装饰器模式**：每个策略增强或修改前一个策略的行为
- **顺序重要**：后应用的策略会覆盖前面的结果
- **LimitedBeadingStrategy必须最后**：因为它添加0宽度标记线

## 实际应用流程

### 1. 输入参数
```cpp
coord_t thickness = 2400;  // 2.4mm厚度
coord_t bead_count = 3;    // 期望3条线
```

### 2. 策略计算
```cpp
auto strategy = BeadingStrategyFactory::makeStrategy(...);
Beading result = strategy->compute(thickness, bead_count);
```

### 3. 输出结果
```cpp
// result.bead_widths = [400, 800, 400]     // 外-内-外线宽
// result.toolpath_locations = [200, 1200, 2200]  // 线中心位置
// result.total_thickness = 2400
// result.left_over = 0
```

## 关键算法详解

### 过渡厚度计算
```cpp
coord_t getTransitionThickness(coord_t lower_bead_count) const {
    const coord_t lower_ideal = getOptimalThickness(lower_bead_count);
    const coord_t higher_ideal = getOptimalThickness(lower_bead_count + 1);
    const Ratio threshold = (lower_bead_count % 2 == 1) ? 
                           wall_split_middle_threshold_ : wall_add_middle_threshold_;
    return lower_ideal + threshold * (higher_ideal - lower_ideal);
}
```

### 最优线数计算
```cpp
coord_t getOptimalBeadCount(coord_t thickness) const {
    const coord_t naive_count = thickness / optimal_width_;
    const coord_t remainder = thickness - naive_count * optimal_width_;
    const coord_t minimum_width = optimal_width_ * threshold;
    return naive_count + (remainder >= minimum_width);
}
```

## 参数控制与效果

### narrow_area_merge参数
- **true**：启用WideningBeadingStrategy，窄区域转换为单线
- **false**：跳过WideningBeadingStrategy，窄区域变成孔洞

### 阈值参数
- **wall_split_middle_threshold_**：控制何时分割中间线
- **wall_add_middle_threshold_**：控制何时添加新线
- **min_feature_size**：最小特征尺寸，影响窄区域处理

## 总结

BeadingStrategy系统是CuraEngine中最核心的算法之一，它：

1. **解决核心问题**：将任意厚度转换为可打印的线宽和位置
2. **模块化设计**：通过策略模式实现不同算法的组合
3. **灵活配置**：支持多种参数控制不同的打印效果
4. **质量优化**：通过外墙稳定、窄区域处理等提高打印质量
5. **扩展性强**：易于添加新的策略和算法

这个系统的设计体现了软件工程中的多个重要原则：单一职责、开闭原则、装饰器模式和工厂模式，是一个优秀的算法架构实现。

## 深入技术细节

### 权重分配算法（DistributedBeadingStrategy）

**高斯分布权重函数**：
```cpp
const auto getWeight = [middle, this](coord_t bead_idx) {
    const double dev_from_middle = bead_idx - middle;
    return std::max(0.0, 1.0 - one_over_distribution_radius_squared_ * dev_from_middle * dev_from_middle);
};
```

**分配原理**：
- 中间线获得最高权重（权重=1.0）
- 距离中心越远，权重越低
- 外线权重最低，保持相对稳定
- 总权重归一化，确保厚度守恒

**实际效果示例**：
```
厚度=2.5mm，3条线，最优宽度=0.4mm
额外厚度 = 2.5 - 3*0.4 = 1.3mm

权重分配：[0.2, 1.0, 0.2] (归一化后)
最终宽度：[0.58mm, 1.24mm, 0.58mm]
位置：[0.29mm, 1.25mm, 2.21mm]
```

### 过渡区域处理

**过渡厚度的物理意义**：
- 决定何时从n条线过渡到n+1条线
- 避免频繁的线数变化
- 提供平滑的宽度过渡

**阈值参数的作用**：
```cpp
// 奇数线数：使用split阈值（分割现有线）
// 偶数线数：使用add阈值（添加新线）
const Ratio threshold = (lower_bead_count % 2 == 1) ?
                       wall_split_middle_threshold_ : wall_add_middle_threshold_;
```

### 对称性保证

**LimitedBeadingStrategy中的对称性算法**：
```cpp
// 强制对称性
if (bead_count % 2 == 1) {
    ret.toolpath_locations[bead_count / 2] = thickness / 2;  // 中心线居中
}
for (coord_t bead_idx = 0; bead_idx < (bead_count + 1) / 2; bead_idx++) {
    ret.toolpath_locations[bead_count - 1 - bead_idx] = thickness - ret.toolpath_locations[bead_idx];
}
```

**对称性的重要性**：
- 确保模型尺寸精度
- 避免偏心导致的强度不均
- 提供视觉上的一致性

## 性能优化与边界条件

### 特殊情况处理

**1. 单线情况（bead_count = 1）**：
```cpp
const coord_t outer_width = thickness;
ret.bead_widths.emplace_back(outer_width);
ret.toolpath_locations.emplace_back(outer_width / 2);  // 居中
```

**2. 双线情况（bead_count = 2）**：
```cpp
const coord_t outer_width = thickness / 2;  // 平均分配
ret.bead_widths.emplace_back(outer_width);
ret.bead_widths.emplace_back(outer_width);
ret.toolpath_locations.emplace_back(outer_width / 2);
ret.toolpath_locations.emplace_back(thickness - outer_width / 2);
```

**3. 零线情况（bead_count = 0）**：
```cpp
ret.left_over = thickness;  // 全部标记为剩余
```

### 数值稳定性

**浮点精度处理**：
- 使用coord_t（通常为int64_t）避免浮点误差
- 微米级精度（1μm = 1 coord_t单位）
- 权重计算使用double，最终转换为coord_t

**边界检查**：
```cpp
assert(std::abs(int(from_junctions.size()) - int(to_junctions.size())) <= 1);
```

## 实际应用案例

### 案例1：标准外墙打印
```
输入：厚度=1.6mm，期望2条线，最优宽度=0.4mm
策略链：Distributed → Redistribute → Limited

结果：
- 外墙：0.4mm（固定）
- 内墙：1.2mm（承担额外厚度）
- 位置：[0.2mm, 1.4mm]
```

### 案例2：窄区域处理
```
输入：厚度=0.3mm，min_feature_size=0.2mm，min_bead_width=0.1mm
策略：WideningBeadingStrategy

结果：
- 单线宽度：max(0.3mm, 0.1mm) = 0.3mm
- 位置：[0.15mm]
- 避免了该区域被删除
```

### 案例3：超厚墙体
```
输入：厚度=5.0mm，max_bead_count=6，最优宽度=0.4mm
策略：Limited限制最大线数

结果：
- 6条正常线 + 2条0宽度标记线
- 剩余厚度分配给中间线
- 0宽度线标记内边界，供填充参考
```

## 调试与优化建议

### 关键日志信息
```cpp
spdlog::debug("Applying the Distributed Beading strategy");
spdlog::debug("Applying the Redistribute meta-strategy with outer-wall width = {}", outer_width);
spdlog::debug("Applying the Widening Beading meta-strategy with min_input = {}", min_input);
spdlog::debug("Applying the Limited Beading meta-strategy with max_count = {}", max_count);
```

### 参数调优指南

**1. 外墙质量优化**：
- 增大`wall_split_middle_threshold_`：减少外墙宽度变化
- 使用RedistributeBeadingStrategy：固定外墙宽度

**2. 细节保持**：
- 启用`narrow_area_merge=true`
- 减小`min_feature_size`：保留更小的特征
- 增大`min_bead_width`：确保可打印性

**3. 性能优化**：
- 减小`max_bead_count`：限制计算复杂度
- 增大`default_transition_length_`：减少过渡区域

### 常见问题与解决方案

**问题1：外墙宽度不稳定**
- 原因：没有使用RedistributeBeadingStrategy
- 解决：确保策略链包含重分配策略

**问题2：细小特征丢失**
- 原因：WideningBeadingStrategy未启用
- 解决：设置`narrow_area_merge=true`

**问题3：内存使用过高**
- 原因：max_bead_count设置过大
- 解决：根据实际需要限制最大线数

**问题4：过渡区域不平滑**
- 原因：transition_length设置过小
- 解决：增大过渡长度参数

## 扩展开发指南

### 添加新策略的步骤

**1. 继承BeadingStrategy基类**：
```cpp
class CustomBeadingStrategy : public BeadingStrategy {
public:
    Beading compute(coord_t thickness, coord_t bead_count) const override;
    coord_t getOptimalBeadCount(coord_t thickness) const override;
    // 实现其他虚函数...
};
```

**2. 实现核心算法**：
- compute()：核心计算逻辑
- getOptimalBeadCount()：最优线数计算
- 其他辅助函数

**3. 在工厂中集成**：
```cpp
// 在BeadingStrategyFactory::makeStrategy中添加
if (use_custom_strategy) {
    ret = make_unique<CustomBeadingStrategy>(std::move(ret), ...);
}
```

### 策略设计原则

**1. 单一职责**：每个策略只解决一个特定问题
**2. 链式兼容**：必须能够包装其他策略
**3. 参数验证**：检查输入参数的有效性
**4. 性能考虑**：避免不必要的计算
**5. 数值稳定**：处理边界条件和精度问题

这个BeadingStrategy系统是3D打印路径规划的核心，理解其工作原理对于优化打印质量和开发新功能至关重要。
