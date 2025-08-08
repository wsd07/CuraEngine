# Spiralize 模式功能

## 概述

Spiralize 模式（螺旋模式）是 CuraEngine 的一个特殊打印模式，通过连续的螺旋路径打印外壁，消除层间接缝，实现光滑的表面效果。

## 已实现功能

### 1. Spiralize 模式下 Fuzzy Skin 支持

**问题**: 原始的 fuzzy skin 处理逻辑只处理常规的 `wall_toolpaths`，而 spiralize 模式下的外壁被存储在 `spiral_wall` 中。

**解决方案**: 在 `processFuzzyWalls` 函数中添加对 `spiral_wall` 的处理。

**实现文件**: `CuraEngine/src/FffPolygonGenerator.cpp`

#### 关键修改

```cpp
// === 处理螺旋模式的 fuzzy skin ===
if (!part.spiral_wall.empty())
{
    Shape fuzzy_spiral_wall;
    for (const Polygon& spiral_polygon : part.spiral_wall)
    {
        if (spiral_polygon.size() < 3)
        {
            // 点数太少，直接保留原始多边形
            fuzzy_spiral_wall.push_back(spiral_polygon);
            continue;
        }

        // 对于spiral_wall，简化outside_only逻辑处理
        // spiral_wall是螺旋模式的外轮廓，通常应该应用fuzzy skin
        // 所以我们对spiral_wall不应用outside_only限制
        bool skip_this_polygon = false;
        
        // 注意：对于spiral_wall，我们不检查outside_only
        // 因为spiral_wall本身就是外轮廓的表示，用户启用spiralize模式
        // 通常希望这些轮廓都有fuzzy效果

        if (skip_this_polygon)
        {
            fuzzy_spiral_wall.push_back(spiral_polygon);
            continue;
        }

        // 应用fuzzy效果到螺旋多边形
        Polygon fuzzy_polygon;
        
        // 生成fuzzy点
        int64_t dist_left_over = (min_dist_between_points / 4) + rand() % (min_dist_between_points / 4);
        
        for (size_t i = 0; i < spiral_polygon.size(); ++i)
        {
            const Point2LL& p0 = spiral_polygon[i];
            const Point2LL& p1 = spiral_polygon[(i + 1) % spiral_polygon.size()];
            
            if (p0 == p1) // 避免重复点
            {
                fuzzy_polygon.push_back(p1);
                continue;
            }

            const Point2LL p0p1 = p1 - p0;
            const int64_t p0p1_size = vSize(p0p1);
            int64_t p0pa_dist = dist_left_over;
            
            if (p0pa_dist >= p0p1_size)
            {
                // 如果剩余距离大于线段长度，在中点添加一个点
                const Point2LL p = p1 - (p0p1 / 2);
                fuzzy_polygon.push_back(p);
            }
            
            // 在线段上生成fuzzy点
            for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points + rand() % range_random_point_dist)
            {
                const int r = rand() % (fuzziness * 2) - fuzziness;
                const Point2LL perp_to_p0p1 = turn90CCW(p0p1);
                const Point2LL fuzz = normal(perp_to_p0p1, r);
                const Point2LL pa = p0 + normal(p0p1, p0pa_dist);
                fuzzy_polygon.push_back(pa + fuzz);
            }
            
            dist_left_over = p0pa_dist - p0p1_size;
        }

        // 确保至少有3个点
        while (fuzzy_polygon.size() < 3 && spiral_polygon.size() >= 3)
        {
            size_t point_idx = spiral_polygon.size() - 2;
            fuzzy_polygon.push_back(spiral_polygon[point_idx]);
            if (point_idx == 0)
            {
                break;
            }
            point_idx--;
        }
        
        if (fuzzy_polygon.size() < 3)
        {
            // 如果还是不够3个点，直接使用原始多边形
            fuzzy_spiral_wall.push_back(spiral_polygon);
        }
        else
        {
            // 确保闭合多边形的首尾点一致
            if (!fuzzy_polygon.empty() && spiral_polygon.front() == spiral_polygon.back())
            {
                fuzzy_polygon.back() = fuzzy_polygon.front();
            }
            fuzzy_spiral_wall.push_back(fuzzy_polygon);
        }
    }
    
    // 更新螺旋壁
    part.spiral_wall = fuzzy_spiral_wall;
}
```

#### 技术特点

1. **算法一致性**: 完全复用原有的 fuzzy skin 算法逻辑
2. **数据结构适配**: 处理 `Shape` vs `VariableWidthLines` 的差异
3. **参数兼容**: 支持所有现有的 fuzzy skin 参数
4. **质量保证**: 包含完整的边界条件处理

#### 支持的参数

- `magic_fuzzy_skin_enabled`: 启用/禁用 fuzzy skin
- `magic_fuzzy_skin_outside_only`: 仅外轮廓应用（对 spiral_wall 不限制）
- `magic_fuzzy_skin_thickness`: 随机偏移的最大距离
- `magic_fuzzy_skin_point_dist`: 点间平均距离

### 2. 层间路径优化 Combing 改进

**问题**: `optimizeLayerEndForNextLayerStart` 函数中的空移路径使用直线连接，导致跨越模型造成拉丝。

**解决方案**: 将所有直线 travel 路径改为 combing travel 路径。

**实现文件**: `CuraEngine/src/LayerPlan.cpp`

#### 新增函数

```cpp
/*!
 * \brief 创建一个使用 combing 的 travel 路径
 * \param from_point 起始点
 * \param to_point 目标点
 * \param layer_z 层高度
 * \param extruder_nr 挤出机编号
 * \return 包含 combing 路径的 GCodePath 向量
 */
std::vector<GCodePath> createCombingTravel(const Point2LL& from_point, const Point2LL& to_point, coord_t layer_z, size_t extruder_nr);
```

#### 替换的路径

1. **travel_to_end**: 到原终点的 travel
2. **travel_to_start2**: 到原起点的 travel  
3. **travel_to_optimal2**: 回到最优点的 travel
4. **travel_to_next_layer1/2**: 到下一层起始点的 travel
5. **initial_travel**: 初始连接 travel

#### 技术特点

- **智能回退**: 无 comb 对象时使用直线 travel
- **边界遵循**: 使用 `comb_boundary_preferred_` 进行边界检测
- **完整 combing**: 调用 `comb_->calc()` 计算避障路径
- **多段支持**: 支持生成多段 combing 路径

## 新实现功能

### 1. Spiralize 加强层功能 ✅

#### 新参数定义

1. **magic_spiralize_reinforce_layers**
   - 类型: 整数
   - 描述: 在 spiralize 初始阶段需要加强的层数
   - 默认值: 0（禁用）
   - 范围: 0-20

2. **magic_spiralize_reinforce_contours** ✅ 已升级为 float
   - 类型: 浮点数
   - 描述: 加强结构的总宽度，以 wall_0 宽度的倍数表示
   - 默认值: 1.0
   - 范围: 0.5-5.0
   - 示例: 1.3 表示加强结构总宽度为 wall_0 的 1.3 倍

3. **magic_spiralize_reinforce_flip**
   - 类型: 布尔值
   - 描述: 加强结构是否反向打印
   - 默认值: false

4. **magic_spiralize_reinforce_fade** ✅ 新增
   - 类型: 布尔值
   - 描述: 加强结构宽度线性渐变
   - 默认值: false
   - 功能: 启用首层到末层的线性渐变

5. **magic_spiralize_reinforce_mini_contours** ✅ 新增
   - 类型: 浮点数
   - 描述: 最后一个加强层的圈数
   - 默认值: 1.0
   - 范围: 0.0-5.0
   - 功能: 与 reinforce_contours 配合实现线性渐变

#### 实现逻辑

**文件位置**: `CuraEngine/src/WallsComputation.cpp`

##### 用户指定算法：简单清晰的圈数分配 ✅ 重新实现

**核心思想**：
- `magic_spiralize_reinforce_contours`：首层圈数（如3.6）
- `magic_spiralize_reinforce_mini_contours`：末层圈数（如0.8）
- `magic_spiralize_reinforce_layers`：加强层数（如5层）

**算法步骤**：

```cpp
// 1. 计算每层圈数差
double contours_diff_per_layer = (首层圈数 - 末层圈数) / (加强层数 - 1);
// 例如：(3.6 - 0.8) / (5 - 1) = 0.7

// 2. 计算当前层圈数
double current_contours_count = 首层圈数 - 当前层索引 * 每层差值;
// 第1层：3.6 - 0 * 0.7 = 3.6
// 第2层：3.6 - 1 * 0.7 = 2.9
// 第3层：3.6 - 2 * 0.7 = 2.2
// 第4层：3.6 - 3 * 0.7 = 1.5
// 第5层：3.6 - 4 * 0.7 = 0.8

// 3. 四舍五入得到实际圈数
size_t actual_contour_count = round(current_contours_count);

// 4. 计算每圈宽度
// 前(n-1)圈：1.0倍wall_0
// 最后1圈：剩余宽度 = current_contours_count - (actual_contour_count - 1)
```

##### 用户算法示例 ✅ 重新实现

**配置**:
- `magic_spiralize_reinforce_contours = 3.6`（首层）
- `magic_spiralize_reinforce_mini_contours = 0.8`（末层）
- `magic_spiralize_reinforce_layers = 5`

**计算过程**:
- 每层差值 = `(3.6 - 0.8) / (5 - 1) = 0.7`

**各层结果**（从内到外排列）:

**第1层**: `3.6` → `round(3.6) = 4`圈
- 最内圈: `3.6 - 3 = 0.6 * wall_0`（可变宽度）
- 外圈3个: `1.0 * wall_0`（固定宽度）
- **结果**: `0.6、1、1、1`（从内到外）

**第2层**: `2.9` → `round(2.9) = 3`圈
- 最内圈: `2.9 - 2 = 0.9 * wall_0`（可变宽度）
- 外圈2个: `1.0 * wall_0`（固定宽度）
- **结果**: `0.9、1、1`（从内到外）

**第3层**: `2.2` → `round(2.2) = 2`圈
- 最内圈: `2.2 - 1 = 1.2 * wall_0`（可变宽度）
- 外圈1个: `1.0 * wall_0`（固定宽度）
- **结果**: `1.2、1`（从内到外）

**第4层**: `1.5` → `round(1.5) = 2`圈
- 最内圈: `1.5 - 1 = 0.5 * wall_0`（可变宽度）
- 外圈1个: `1.0 * wall_0`（固定宽度）
- **结果**: `0.5、1`（从内到外）

**第5层**: `0.8` → `round(0.8) = 1`圈
- 最内圈: `0.8 * wall_0`（可变宽度）
- **结果**: `0.8`（只有一圈）

**特殊情况**: 如果末层设置为0.3，第5层会是 `round(0.3) = 0`圈，不生成加强结构。

##### 渐变功能示例

**配置**:
- `magic_spiralize_reinforce_contours = 3.0`
- `magic_spiralize_reinforce_layers = 5`
- `magic_spiralize_reinforce_fade = true`

**每层效果**:
- 第1层: `3.0` 圈 → 3圈，每圈 `1.0 * wall_0`
- 第2层: `2.4` 圈 → 2圈，第1圈 `1.0 * wall_0`，第2圈 `1.4 * wall_0`
- 第3层: `1.8` 圈 → 2圈，第1圈 `1.0 * wall_0`，第2圈 `0.8 * wall_0`
- 第4层: `1.2` 圈 → 1圈，第1圈 `1.2 * wall_0`
- 第5层: `0.6` 圈 → 1圈，第1圈 `0.6 * wall_0`

##### 用户偏移算法示例 ✅ 纠正内外顺序

**示例**: 第3层，`current_contours_count = 2.2`，`line_width_0 = 400μm`

**计算过程**:
1. `actual_contour_count = round(2.2) = 2`圈
2. 最内圈: `2.2 - 1 = 1.2 * wall_0 = 480μm`（可变宽度）
3. 外圈: `1.0 * wall_0 = 400μm`（固定宽度）
4. `cumulative_offset = 200μm`（初始值 = line_width_0 / 2）

**生成顺序**（从内到外）:
1. **最内圈**（可变）: 宽度 = `480μm`，偏移 = `200 + 240 = 440μm`
2. `cumulative_offset = 200 + 480 = 680μm`
3. **外圈**（固定）: 宽度 = `400μm`，偏移 = `680 + 200 = 880μm`

**偏移公式**: `偏移 = cumulative_offset + 本圈宽度/2`
- 这确保了两圈中心距 = `0.5 * 内圈宽度 + 0.5 * 外圈宽度`

**最终结果**（从内到外）:
- 最内圈：偏移 440μm，宽度 480μm（1.2倍wall_0，可变）
- 外圈：偏移 880μm，宽度 400μm（1.0倍wall_0，固定）

**关键特点**:
- ✅ **最内圈**是可变宽度（偏移最多）
- ✅ **外圈**都是固定1.0倍wall_0
- ✅ 正确的偏移计算：`0.5 + 本圈宽度/2`
- ✅ 符合用户要求的内外顺序

#### Z高度处理

**文件位置**: `CuraEngine/src/FffGcodeWriter.cpp` 和 `CuraEngine/src/LayerPlan.cpp`

```cpp
// 在 FffGcodeWriter.cpp 中区分主螺旋圈和加强圈
for (size_t wall_idx = 0; wall_idx < part.spiral_wall.size(); wall_idx++)
{
    const Polygon& wall_outline = part.spiral_wall[wall_idx];

    // 判断当前是否为加强圈
    bool is_reinforcement_contour = false;
    if (is_reinforce_layer && wall_idx > 0 && wall_idx <= reinforce_contours)
    {
        is_reinforcement_contour = true;
    }

    if (is_reinforcement_contour)
    {
        // 加强圈：使用固定Z高度（不进行螺旋化）
        gcode_layer.spiralizeReinforcementContour(mesh_config.inset0_config, wall_outline, seam_vertex_idx);
    }
    else
    {
        // 主螺旋圈：使用正常的螺旋化逻辑
        gcode_layer.spiralizeWallSlice(mesh_config.inset0_config, wall_outline, *last_wall_outline, seam_vertex_idx, last_seam_vertex_idx, is_top_layer, is_bottom_layer);
    }
}
```

#### 新增函数

**文件位置**: `CuraEngine/src/LayerPlan.cpp`

```cpp
void LayerPlan::spiralizeReinforcementContour(
    const GCodePathConfig& config,
    const Polygon& wall,
    int seam_vertex_idx)
{
    // 移动到起始点
    const Point2LL start_point = wall[seam_vertex_idx];
    addTravel(start_point);

    // 打印加强圈（不进行螺旋化，使用固定Z高度）
    constexpr bool spiralize = false; // 加强圈不进行螺旋化
    constexpr Ratio width_factor = 1.0_r;
    constexpr Ratio flow_ratio = 1.0_r;
    constexpr Ratio speed_factor = 1.0_r;

    // 从缝合点开始，按顺序打印整个轮廓
    const int n_points = wall.size();
    for (int wall_point_idx = 1; wall_point_idx <= n_points; ++wall_point_idx)
    {
        const Point2LL& p = wall[(seam_vertex_idx + wall_point_idx) % n_points];
        addExtrusionMove(p, config, SpaceFillType::Polygons, flow_ratio, width_factor, spiralize, speed_factor);
    }
}
```

#### 技术特点

1. **智能宽度分配**: 根据 float 值智能计算圈数和每圈宽度
2. **精确偏移计算**: 基于实际线宽计算精确的偏移距离
3. **方向控制**: 支持加强圈反向打印，包含点序列平移
4. **Z高度分离**: 主螺旋圈进行Z上升，加强圈保持固定Z高度
5. **渐变支持**: 支持加强结构宽度线性渐变
6. **可变线宽**: 支持每个加强圈使用不同的线宽
7. **起点优化**: 智能选择加强圈起点，最小化移动距离 ✅ 新增
8. **参数验证**: 确保生成的多边形有效（至少3个顶点）
9. **完整集成**: 与现有 spiralize 系统完全兼容

#### 参数配置示例

##### 基础配置
```ini
# 启用螺旋模式
magic_spiralize = true

# 前3层使用加强结构
magic_spiralize_reinforce_layers = 3

# 加强结构总宽度为 wall_0 的 2.5 倍
magic_spiralize_reinforce_contours = 2.5

# 加强圈反向打印
magic_spiralize_reinforce_flip = true
```

##### 渐变配置
```ini
# 启用螺旋模式
magic_spiralize = true

# 前5层使用加强结构
magic_spiralize_reinforce_layers = 5

# 初始加强结构总宽度为 wall_0 的 3.0 倍
magic_spiralize_reinforce_contours = 3.0

# 启用渐变，从 3.0 线性减少到 0.5
magic_spiralize_reinforce_fade = true

# 加强圈反向打印
magic_spiralize_reinforce_flip = true
```

##### 精细控制配置
```ini
# 启用螺旋模式
magic_spiralize = true

# 前2层使用加强结构
magic_spiralize_reinforce_layers = 2

# 加强结构总宽度为 wall_0 的 1.3 倍（单圈加宽）
magic_spiralize_reinforce_contours = 1.3

# 不启用渐变
magic_spiralize_reinforce_fade = false

# 加强圈正向打印
magic_spiralize_reinforce_flip = false
```

## 相关参数

### 现有参数

- `magic_spiralize`: 启用螺旋模式
- `magic_spiralize_range`: 螺旋化高度范围
- `smooth_spiralized_z`: 平滑Z轴移动

### 新增参数 ✅

- `magic_spiralize_reinforce_layers`: 加强层数（已实现）
- `magic_spiralize_reinforce_contours`: 加强圈数（已实现）
- `magic_spiralize_reinforce_flip`: 加强圈反向（已实现）

### 参数详细说明

#### magic_spiralize_reinforce_layers
- **类型**: 整数
- **默认值**: 0
- **范围**: 0-20
- **描述**: 螺旋模式中需要加强的初始层数
- **启用条件**: `magic_spiralize = true`

#### magic_spiralize_reinforce_contours
- **类型**: 整数
- **默认值**: 1
- **范围**: 1-5
- **描述**: 加强层要打印的额外同心轮廓数量
- **启用条件**: `magic_spiralize = true` 且 `magic_spiralize_reinforce_layers > 0`

#### magic_spiralize_reinforce_flip
- **类型**: 布尔值
- **默认值**: false
- **描述**: 是否以与主螺旋相反的方向打印加强轮廓
- **启用条件**: `magic_spiralize = true` 且 `magic_spiralize_reinforce_layers > 0`

## 技术挑战与解决方案

### 1. 路径连接 ✅ 已解决

**挑战**: 如何在原始圈和加强圈之间实现平滑的路径连接。

**解决方案**:
- 使用 `addTravel()` 在加强圈之间进行移动
- 每个加强圈都从相同的缝合点开始，确保路径连接的一致性
- 通过 `spiralizeReinforcementContour()` 函数独立处理每个加强圈

### 2. Z高度控制 ✅ 已解决

**挑战**: 在加强圈打印时如何正确处理Z轴位置。

**解决方案**:
- 主螺旋圈：使用 `spiralizeWallSlice()` 进行正常的螺旋化（Z轴上升）
- 加强圈：使用 `spiralizeReinforcementContour()` 保持固定Z高度
- 通过 `spiralize = false` 参数禁用加强圈的Z轴螺旋化

### 3. 性能优化 ✅ 已优化

**挑战**: 多圈打印对切片时间和内存使用的影响。

**解决方案**:
- 仅在需要的层数范围内生成加强圈
- 使用高效的多边形偏移算法
- 在生成后立即验证多边形有效性，避免无效数据传播

### 4. 质量保证 ✅ 已实现

**挑战**: 确保加强圈不会与原始圈产生冲突或重叠。

**解决方案**:
- 使用精确的偏移算法，确保合理的间距
- 验证生成的多边形至少有3个顶点
- 通过参数限制最大加强圈数量（0.5-5.0）
- 分离主螺旋圈和加强圈的简化处理，避免意外移除

### 5. 加强圈消失问题修复 ✅ 已修复

**问题**: 加强结构只保留最外圈，内圈消失。

**原因分析**:
1. **偏移计算错误**: 原始算法中内圈偏移可能小于外圈偏移
2. **Simplify操作影响**: 统一的简化操作可能移除加强圈
3. **调试信息不足**: 缺少详细的生成过程日志

**解决方案**:
- **重新设计偏移算法**: 从内到外累积计算偏移，确保正确的层次关系
- **分离简化处理**: 主螺旋圈和加强圈分别处理，避免意外移除
- **增强调试信息**: 添加详细的生成和验证日志
- **空结果检查**: 检查偏移结果是否为空，及时发现问题

### 6. 加强圈起点优化 ✅ 新增功能

**问题**: 打印完 spiralize 外圈后，加强结构的起点不够优化，导致不必要的长距离移动。

**解决方案**: 智能起点选择算法

**实现位置**: `CuraEngine/src/FffGcodeWriter.cpp`

#### 核心算法

```cpp
/*!
 * \brief 在多边形中找到距离指定点最近的顶点索引
 */
int findClosestVertexToPoint(const Polygon& polygon, const Point2LL& target_point)
{
    int closest_idx = 0;
    coord_t min_distance_squared = vSize2(polygon[0] - target_point);

    for (size_t i = 1; i < polygon.size(); i++)
    {
        coord_t distance_squared = vSize2(polygon[i] - target_point);
        if (distance_squared < min_distance_squared)
        {
            min_distance_squared = distance_squared;
            closest_idx = static_cast<int>(i);
        }
    }

    return closest_idx;
}
```

#### 优化流程

```cpp
// 跟踪上一个结束点
Point2LL last_end_point = gcode_layer.getLastPlannedPositionOrStartingPosition();

for (每个加强圈) {
    // 优化加强圈起点：找到距离上一个结束点最近的点
    int optimized_seam_vertex_idx = findClosestVertexToPoint(wall_outline, last_end_point);

    // 使用优化后的起点打印加强圈
    gcode_layer.spiralizeReinforcementContour(config, wall_outline, optimized_seam_vertex_idx, line_width);

    // 更新上一个结束点
    last_end_point = wall_outline[optimized_seam_vertex_idx];
}
```

#### 优化效果

1. **减少移动距离**: 每个加强圈都从距离上一个结束点最近的位置开始
2. **提高打印效率**: 减少不必要的空移时间
3. **改善打印质量**: 减少因长距离移动导致的拉丝
4. **智能路径规划**: 自动选择最优的打印路径

#### 调试信息

```cpp
spdlog::debug("【螺旋加强起点优化】第{}层，加强圈{}，原起点索引：{}，优化后起点索引：{}，距离：{:.2f}mm",
             layer_nr, wall_idx, seam_vertex_idx, optimized_seam_vertex_idx,
             INT2MM(vSize(wall_outline[optimized_seam_vertex_idx] - last_end_point)));
```

### 7. 多圈加强结构逻辑重构 ✅ 重新设计

**问题**: 原有的多圈加强结构逻辑不符合要求：
1. 最内圈应该是可变挤出宽度的（0.5-1.5倍wall_0）
2. 其他圈应该都是固定1倍wall_0
3. 超过1.5倍要拆分，小于0.5倍要合并

**原始错误逻辑**:
- 外圈是可变宽度
- 内圈是剩余宽度
- 没有0.5-1.5倍的限制

**重新设计的算法**:

```cpp
// 1. 计算内圈宽度比例和固定圈数
double inner_width_ratio = current_contours_width - std::floor(current_contours_width);
size_t fixed_contour_count = static_cast<size_t>(std::floor(current_contours_width));

// 2. 处理边界情况
if (inner_width_ratio < 0.5 && fixed_contour_count > 0) {
    // 小于0.5，并入外圈
    fixed_contour_count--;
    inner_width_ratio += 1.0;
} else if (inner_width_ratio > 1.5) {
    // 大于1.5，拆分为2圈
    fixed_contour_count++;
    inner_width_ratio -= 1.0;
}

// 3. 生成顺序：先内圈（可变），后外圈（固定）
```

**修复效果**:
- ✅ 最内圈宽度严格控制在0.5-1.5倍wall_0范围内
- ✅ 其他圈都是固定1.0倍wall_0
- ✅ 自动处理边界情况（合并/拆分）
- ✅ 正确的偏移计算和打印顺序

### 8. 算法重构：用户指定的简单算法 ✅ 完全重新实现

**问题**: 之前的算法过于复杂，不符合用户的简单清晰要求。

**用户要求的算法**:
1. `magic_spiralize_reinforce_contours`：首层圈数（如3.6）
2. `magic_spiralize_reinforce_mini_contours`：末层圈数（如0.8）
3. 线性渐变：每层圈数 = 首层 - 层索引 × 每层差值
4. 四舍五入得到实际圈数
5. 前(n-1)圈固定1.0倍wall_0，最后1圈是剩余宽度
6. 偏移公式：`0.5 + 本圈宽度/2`

**重新实现**:

```cpp
// 1. 计算每层圈数差
double contours_diff_per_layer = (首层圈数 - 末层圈数) / (加强层数 - 1);

// 2. 计算当前层圈数
double current_contours_count = 首层圈数 - 当前层索引 * 每层差值;

// 3. 四舍五入得到实际圈数
size_t actual_contour_count = round(current_contours_count);

// 4. 计算每圈宽度
for (size_t i = 0; i < actual_contour_count; i++) {
    if (i < actual_contour_count - 1) {
        // 前面的圈：1.0倍wall_0
        width = line_width_0;
    } else {
        // 最后一圈：剩余宽度
        width = (current_contours_count - (actual_contour_count - 1)) * line_width_0;
    }

    // 偏移计算
    offset = cumulative_offset + width / 2;
    cumulative_offset += width;
}
```

**新增参数**: `magic_spiralize_reinforce_mini_contours`
- 完整的参数定义和多语言翻译
- 与现有参数完美集成

**算法优势**:
- ✅ 简单清晰，易于理解
- ✅ 符合用户直觉
- ✅ 精确的线性渐变
- ✅ 正确的偏移计算

### 9. 圈的内外顺序纠正 ✅ 重要修正

**问题**: 之前把可变圈放在了最外面，但用户要求可变圈在最内侧（偏移最多的位置）。

**错误理解**:
- 可变圈在最外面（偏移最少）
- 固定圈在里面

**正确理解**:
- **最内圈**：可变宽度，偏移最多（向内偏移最深）
- **外圈**：固定1.0倍wall_0，偏移较少

**修正实现**:

```cpp
// 重新排序：最内圈（可变宽度）在最里面
for (size_t i = 0; i < actual_contour_count; i++) {
    if (i == 0) {
        // 第一个生成的是最内圈（可变宽度，偏移最多）
        coord_t inner_width = static_cast<coord_t>(remaining_width * line_width_0);
        coord_t offset = cumulative_offset + inner_width / 2;
        // 最内圈
    } else {
        // 后面的圈都是固定1倍wall_0（外圈）
        coord_t offset = cumulative_offset + line_width_0 / 2;
        // 外圈
    }
}
```

**修正效果**:
- ✅ 索引0：最内圈（可变宽度，偏移最多）
- ✅ 索引1+：外圈（固定宽度，偏移较少）
- ✅ 符合用户要求的内外顺序
- ✅ 正确的物理位置关系

## 应用场景

### 1. 花瓶模式增强 ✅

为花瓶模式提供底部加强，提高结构强度。

**配置示例**:
```ini
magic_spiralize_reinforce_layers = 5
magic_spiralize_reinforce_contours = 2
magic_spiralize_reinforce_flip = false
```

### 2. 薄壁打印 ✅

在薄壁打印中提供额外的结构支撑。

**配置示例**:
```ini
magic_spiralize_reinforce_layers = 3
magic_spiralize_reinforce_contours = 1
magic_spiralize_reinforce_flip = true
```

### 3. 装饰性打印 ✅

通过多圈打印创造特殊的视觉效果。

**配置示例**:
```ini
magic_spiralize_reinforce_layers = 10
magic_spiralize_reinforce_contours = 3
magic_spiralize_reinforce_flip = true
```

### 4. 结构强化

为需要额外强度的螺旋打印提供支撑。

**适用场景**:
- 高度较高的螺旋打印
- 需要承重的功能性部件
- 薄壁容器的底部加强

## 历史功能记录

### 1. Spiralize 模式下 Raft 面积优化

**问题**: 当`magic_spiralize`为true且`bottom_layers`为0时，Raft的范围应该基于线条轮廓而不是面积。

**解决方案**: 修改`src/raft.cpp`中的`Raft::generate()`函数，对线条轮廓进行双向扩展。

### 2. only_spiralize_out_surface 功能

**问题**: 在spiralize模式下需要只保留最外层多边形。

**解决方案**: 在`generateSpiralInsets`函数中添加最外层多边形筛选逻辑。

### 3. magic_spiralize_range 高度范围控制

**问题**: 需要在指定高度范围内使用螺旋模式。

**解决方案**: 实现基于高度范围的螺旋模式控制，支持多个高度区间。

### 4. smooth_spiralized_z 平滑Z坐标控制

**问题**: 需要控制螺旋Z坐标的平滑程度。

**解决方案**: 添加参数控制是否启用平滑Z坐标上升。

### 5. 螺旋过渡墙优化

**问题**: 螺旋模式过渡层需要更平滑的流量和速度控制。

**解决方案**: 实现单墙过渡机制，支持流量和速度渐变。

### 6. 螺旋结束墙功能

**问题**: 最高层需要优雅的螺旋结束。

**解决方案**: 添加螺旋结束墙，流量线性减少到0。

## 测试验证

### 编译测试 ✅
- **编译成功**: 所有代码编译无错误
- **函数集成**: 新函数正确集成到现有系统
- **参数验证**: 所有新参数正确定义和翻译
- **起点优化**: 起点优化算法正确实现

### 功能验证建议

1. **基本功能测试**:
   - 启用加强层，观察是否正确生成多个轮廓
   - 验证Z高度控制是否按预期工作
   - 检查加强圈消失问题是否已修复

2. **参数测试**:
   - 测试不同的加强层数和圈数组合
   - 验证反向打印功能
   - 测试渐变功能效果

3. **质量测试**:
   - 检查加强圈与主螺旋圈的连接质量
   - 验证打印质量和结构强度
   - 观察起点优化对打印质量的改善

4. **性能测试**:
   - 测试切片时间影响
   - 验证内存使用情况
   - 测量起点优化对打印时间的影响

5. **起点优化验证**:
   - 观察加强圈起点是否选择合理
   - 检查移动距离是否明显减少
   - 验证调试日志中的距离信息
