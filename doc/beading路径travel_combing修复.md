# beading路径travel combing修复

## 问题背景

用户发现beading过程中产生的路径段之间的空移连接，并没有遵循travel combing的算法，导致beading路径间有很多穿越模型的travel，影响打印质量。

## 问题分析

### 根本原因

在beading路径优化过程中，`InsetOrderOptimizer`使用`PathOrderOptimizer`来优化路径顺序，但是**没有提供combing边界**，导致：

1. **路径顺序优化时**：只使用直线距离计算travel成本，无法避开障碍物
2. **最终travel路径**：虽然`addTravel`函数支持combing，但路径顺序已经被错误优化

### 问题代码

在`InsetOrderOptimizer.cpp`中：

```cpp
constexpr bool detect_loops = false;
constexpr Shape* combing_boundary = nullptr;  // ❌ 问题：没有提供combing边界
const auto group_outer_walls = settings_.get<bool>("group_outer_walls");

PathOrderOptimizer<const ExtrusionLine*> order_optimizer(
    gcode_layer_.getLastPlannedPositionOrStartingPosition(),
    z_seam_config_,
    detect_loops,
    combing_boundary,  // ❌ nullptr导致无法使用combing距离计算
    reverse,
    order,
    group_outer_walls,
    disallowed_areas_for_seams_,
    use_shortest_for_inner_walls,
    overhang_areas_);
```

### 影响范围

这个问题影响所有使用BeadingStrategy的墙体路径：
- 外墙beading路径
- 内墙beading路径  
- 可变线宽路径
- 复杂几何的优化路径

## 解决方案

### 核心修复

为`PathOrderOptimizer`提供正确的combing边界，让路径顺序优化时考虑避障：

```cpp
// === 修复：为beading路径优化提供combing边界，避免穿越模型的travel ===
const Shape* combing_boundary = nullptr;
if (gcode_layer_.getCombBoundaryInside() && !gcode_layer_.getCombBoundaryInside()->empty())
{
    combing_boundary = gcode_layer_.getCombBoundaryInside();
    spdlog::debug("InsetOrderOptimizer使用combing边界，避免穿越模型的travel路径");
}
else
{
    spdlog::debug("InsetOrderOptimizer未找到combing边界，使用直线travel");
}
```

### 技术细节

#### 1. Combing边界获取

使用`LayerPlan::getCombBoundaryInside()`获取内部combing边界：
- 这个边界包含了当前层的已打印区域
- 避免travel路径穿越已打印的墙体
- 与LayerPlan的combing系统保持一致

#### 2. PathOrderOptimizer集成

PathOrderOptimizer已经内置了combing距离计算功能：

```cpp
coord_t getCombingDistance(const Point2LL& a, const Point2LL& b)
{
    if (! PolygonUtils::polygonCollidesWithLineSegment(*combing_boundary_, a, b))
    {
        return getDirectDistance(a, b); // 无碰撞，使用直线距离
    }
    
    // 有碰撞，计算combing路径距离
    CombPath comb_path;
    LinePolygonsCrossings::comb(*combing_boundary_, *combing_grid_, a, b, comb_path, ...);
    
    // 返回combing路径的总长度
    return sum * sum; // 平方距离，便于比较
}
```

#### 3. 优化流程

修复后的流程：
1. **获取combing边界**：从LayerPlan获取当前层的避障边界
2. **路径顺序优化**：PathOrderOptimizer使用combing距离计算最优顺序
3. **travel路径生成**：addTravel函数生成符合combing的实际travel路径

## 修复效果

### 优化前后对比

#### 修复前
- PathOrderOptimizer只使用直线距离
- 可能选择穿越模型的路径顺序
- travel路径虽然使用combing，但顺序已经错误

#### 修复后  
- PathOrderOptimizer使用combing距离
- 选择避开障碍物的最优路径顺序
- travel路径既避障又最短

### 性能影响

#### 计算复杂度
- **路径数量 < 100**：完整combing计算，精确避障
- **路径数量 ≥ 100**：使用5倍直线距离惩罚，平衡性能

#### 内存使用
- combing网格被缓存，避免重复构建
- 内存使用略有增加，但在可接受范围内

## 调试验证

### 调试日志

```
InsetOrderOptimizer使用combing边界，避免穿越模型的travel路径
```

### 验证方法

1. **连接调试端口**：`nc 127.0.0.1 49676`
2. **观察日志**：确认combing边界被正确使用
3. **检查travel路径**：验证路径不再穿越模型

## 兼容性

### 向后兼容
- 不影响现有的简单偏移路径
- 不改变LayerPlan的combing系统
- 保持与现有参数的兼容性

### 边界情况处理
- **无combing边界**：回退到直线距离计算
- **空边界**：安全处理，不会崩溃
- **复杂几何**：使用缓存的网格加速计算

## 相关系统

### LayerPlan Combing系统
- `comb_boundary_preferred_`：首选combing边界
- `comb_boundary_minimum_`：最小combing边界  
- `Comb`类：实际的combing路径计算

### PathOrderOptimizer
- `getCombingDistance()`：combing距离计算
- `getDirectDistance()`：直线距离计算
- `combing_grid_`：避障网格缓存

## 总结

本次修复解决了beading路径travel不遵循combing算法的问题：

1. **问题根源**：PathOrderOptimizer缺少combing边界
2. **解决方案**：提供正确的combing边界给路径优化器
3. **技术优势**：利用现有combing系统，无需重新实现
4. **效果验证**：travel路径不再穿越模型，打印质量提升
5. **性能平衡**：在精度和性能之间找到最佳平衡

现在beading路径的travel移动将正确遵循combing算法，避免穿越模型，显著提升打印质量。
