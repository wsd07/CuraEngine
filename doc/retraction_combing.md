# CuraEngine Retraction Combing 系统深度分析

## 概述

Retraction Combing（回抽避障）是 CuraEngine 中的一个重要功能，旨在减少不必要的回抽操作，通过智能路径规划在已打印区域内移动，避免在空中移动时产生拉丝。该系统通过复杂的几何算法和路径优化，显著提高打印质量和效率。

## 核心概念

### 1. CombingMode 枚举

**文件位置**: `CuraEngine/include/settings/EnumSettings.h`

```cpp
enum class CombingMode
{
    OFF,                    // 关闭避障，总是回抽
    ALL,                    // 在所有已打印区域内避障
    NO_SKIN,               // 避开表面层，在填充和内壁区域避障
    NO_OUTER_SURFACES,     // 避开外表面，在内部区域避障
    INFILL,                // 仅在填充区域内避障
    PLUGIN,                // 插件模式
};
```

### 2. 相关参数

在 Cura 配置中，retraction_combing 相关的参数包括：

```cpp
// 主要参数
"retraction_combing"                    // 避障模式
"retraction_combing_avoid_distance"    // 避障距离
"retraction_combing_max_distance"      // 最大避障距离

// 相关参数
"retraction_enable"                     // 是否启用回抽
"retraction_min_travel"                 // 最小回抽移动距离
"retraction_hop_enabled"                // 是否启用Z跳跃
"retraction_hop_only_when_collides"    // 仅在碰撞时Z跳跃
"travel_avoid_distance"                 // 移动避让距离
```

## 核心类结构

### 1. Comb 类

**文件位置**: `CuraEngine/include/pathPlanning/Comb.h`

这是避障系统的核心类：

```cpp
class Comb
{
private:
    const SliceDataStorage& storage_;              // 切片数据存储
    const LayerIndex layer_nr_;                    // 当前层号
    coord_t travel_avoid_distance_;                // 移动避让距离
    coord_t offset_from_outlines_;                 // 轮廓偏移距离
    coord_t max_moveInside_distance2_;            // 最大内移距离的平方
    coord_t max_move_inside_distance_enlarged2_;   // 扩大的最大内移距离的平方
    coord_t offset_from_inside_to_outside_;        // 从内部到外部的偏移
    coord_t max_crossing_dist2_;                   // 最大穿越距离的平方

    Shape boundary_inside_minimum_;                // 最小内部边界
    Shape boundary_inside_optimal_;                // 最优内部边界
    std::unique_ptr<LocToLineGrid> inside_loc_to_line_minimum_; // 最小边界的位置到线段映射
    std::unique_ptr<LocToLineGrid> inside_loc_to_line_optimal_; // 最优边界的位置到线段映射

public:
    Comb(const SliceDataStorage& storage,
         const LayerIndex layer_nr,
         const Shape& comb_boundary_inside_minimum,
         const Shape& comb_boundary_inside_optimal,
         coord_t comb_boundary_offset,
         coord_t travel_avoid_distance,
         coord_t move_inside_distance);

    bool calc(bool perform_z_hops,
              bool perform_z_hops_only_when_collides,
              const ExtruderTrain& train,
              Point2LL start_point,
              Point2LL end_point,
              CombPaths& comb_paths,
              bool start_inside,
              bool end_inside,
              coord_t max_comb_distance_ignored,
              bool& unretract_before_last_travel_move);
};
```

### 2. CombPath 和 CombPaths

**文件位置**: `CuraEngine/include/pathPlanning/CombPath.h`

```cpp
class CombPath : public std::vector<Point2LL>
{
public:
    bool cross_boundary = false;  // 是否穿越边界
};

class CombPaths : public std::vector<CombPath>
{
public:
    bool throughAir = false;      // 是否通过空气移动
};
```

### 3. LinePolygonsCrossings 类

**文件位置**: `CuraEngine/include/pathPlanning/LinePolygonsCrossings.h`

负责计算线段与多边形的交点和避障路径：

```cpp
class LinePolygonsCrossings
{
private:
    struct Crossing
    {
        size_t poly_idx_;     // 多边形索引
        coord_t x_;           // 交点x坐标
        size_t point_idx_;    // 点索引
    };

    std::vector<Crossing> crossings_;              // 所有交点
    const Shape& boundary_;                        // 边界多边形
    LocToLineGrid& loc_to_line_grid_;             // 位置到线段的映射网格
    Point2LL start_point_, end_point_;            // 起点和终点
    int64_t dist_to_move_boundary_point_outside_; // 移动边界点到外部的距离
    PointMatrix transformation_matrix_;            // 变换矩阵

public:
    static bool comb(const Shape& boundary,
                     LocToLineGrid& loc_to_line_grid,
                     Point2LL startPoint,
                     Point2LL endPoint,
                     CombPath& combPath,
                     int64_t dist_to_move_boundary_point_outside,
                     int64_t max_comb_distance_ignored,
                     bool fail_on_unavoidable_obstacles);
};
```

## 算法核心逻辑

### 1. 避障决策流程

在 `LayerPlan::addTravel` 函数中的决策逻辑：

```cpp
void LayerPlan::addTravel(const Point2LL& p, const bool force_retract)
{
    // 1. 检查是否启用避障
    const CombingMode combing_mode = settings.get<CombingMode>("retraction_combing");
    if (combing_mode == CombingMode::OFF) {
        // 直接回抽移动
        addTravelMove(p, force_retract);
        return;
    }

    // 2. 检查移动距离是否超过最小回抽距离
    if (last_planned_position_ && 
        shorterThen(*last_planned_position_ - p, retraction_config.retraction_min_travel_distance)) {
        // 距离太短，不需要回抽
        addTravelMove(p, false);
        return;
    }

    // 3. 尝试避障路径计算
    if (comb_ != nullptr && !bypass_combing) {
        CombPaths combPaths;
        bool combed = comb_->calc(
            perform_z_hops,
            perform_z_hops_only_when_collides,
            *extruder,
            *last_planned_position_,
            p,
            combPaths,
            was_inside_,
            is_inside_,
            max_distance_ignored,
            unretract_before_last_travel_move);

        if (combed) {
            // 避障成功，处理避障路径
            processCombedPath(combPaths, p);
            return;
        }
    }

    // 4. 避障失败，执行回抽移动
    addTravelMove(p, true);
}
```

### 2. 边界计算

在 `LayerPlan::computeCombBoundary` 中计算避障边界：

```cpp
Shape LayerPlan::computeCombBoundary(const CombBoundary boundary_type)
{
    Shape comb_boundary;
    const CombingMode mesh_combing_mode = settings.get<CombingMode>("retraction_combing");

    if (mesh_combing_mode == CombingMode::OFF) {
        return comb_boundary;  // 空边界
    }

    // 根据不同的避障模式计算边界
    switch (mesh_combing_mode) {
        case CombingMode::ALL:
            // 包含所有已打印区域
            for (const SliceMeshStorage& mesh : storage_.meshes) {
                const SliceLayer& layer = mesh.layers[layer_nr_];
                for (const SliceLayerPart& part : layer.parts) {
                    comb_boundary.add(part.outline.offset(comb_boundary_offset));
                }
            }
            break;

        case CombingMode::NO_SKIN:
            // 排除表面层
            for (const SliceMeshStorage& mesh : storage_.meshes) {
                const SliceLayer& layer = mesh.layers[layer_nr_];
                for (const SliceLayerPart& part : layer.parts) {
                    Shape part_boundary = part.outline.offset(comb_boundary_offset);
                    part_boundary = part_boundary.difference(part.skin_parts);
                    comb_boundary.add(part_boundary);
                }
            }
            break;

        case CombingMode::INFILL:
            // 仅填充区域
            for (const SliceMeshStorage& mesh : storage_.meshes) {
                const SliceLayer& layer = mesh.layers[layer_nr_];
                for (const SliceLayerPart& part : layer.parts) {
                    comb_boundary.add(part.infill_area);
                }
            }
            break;

        case CombingMode::NO_OUTER_SURFACES:
            // 排除外表面
            // 实现类似逻辑...
            break;
    }

    return comb_boundary;
}
```

### 3. 避障路径计算

`Comb::calc` 函数的核心算法：

```cpp
bool Comb::calc(bool perform_z_hops,
                bool perform_z_hops_only_when_collides,
                const ExtruderTrain& train,
                Point2LL start_point,
                Point2LL end_point,
                CombPaths& comb_paths,
                bool start_inside,
                bool end_inside,
                coord_t max_comb_distance_ignored,
                bool& unretract_before_last_travel_move)
{
    // 1. 检查距离是否太短，可以忽略
    if (shorterThen(end_point - start_point, max_comb_distance_ignored)) {
        return true;
    }

    // 2. 确定起点和终点是否在边界内
    bool start_inside_boundary = boundary_inside_optimal_.inside(start_point, true);
    bool end_inside_boundary = boundary_inside_optimal_.inside(end_point, true);

    // 3. 根据起点和终点位置选择不同的避障策略
    if (start_inside_boundary && end_inside_boundary) {
        // 两点都在内部，使用内部避障
        return calcInsideToInside(start_point, end_point, comb_paths, 
                                max_comb_distance_ignored);
    } else if (!start_inside_boundary && !end_inside_boundary) {
        // 两点都在外部，检查是否需要避障
        return calcOutsideToOutside(start_point, end_point, comb_paths);
    } else {
        // 一个在内部，一个在外部，需要穿越边界
        return calcInsideToOutside(start_point, end_point, comb_paths,
                                 start_inside_boundary, max_comb_distance_ignored);
    }
}
```

### 4. 线段多边形交点算法

`LinePolygonsCrossings::comb` 的核心算法：

```cpp
bool LinePolygonsCrossings::generateCombingPath(CombPath& combPath, 
                                               int64_t max_comb_distance_ignored, 
                                               bool fail_on_unavoidable_obstacles)
{
    // 1. 检查线段是否与边界碰撞
    if (!lineSegmentCollidesWithBoundary()) {
        // 无碰撞，直接连接
        return true;
    }

    // 2. 计算扫描线交点
    if (!calcScanlineCrossings(fail_on_unavoidable_obstacles)) {
        return false;
    }

    // 3. 生成基础避障路径
    generateBasicCombingPath(combPath);

    // 4. 优化路径
    CombPath optimized_comb_path;
    bool path_crosses_boundary = optimizePath(combPath, optimized_comb_path);
    
    if (path_crosses_boundary && fail_on_unavoidable_obstacles) {
        return false;
    }

    combPath = optimized_comb_path;
    return true;
}
```

### 5. 坐标变换算法

为了简化计算，系统将问题转换到二维坐标系：

```cpp
bool LinePolygonsCrossings::lineSegmentCollidesWithBoundary()
{
    // 1. 计算变换矩阵，使线段与x轴对齐
    Point2LL diff = end_point_ - start_point_;
    transformation_matrix_ = PointMatrix(diff);
    
    // 2. 变换起点和终点
    transformed_start_point_ = transformation_matrix_.apply(start_point_);
    transformed_end_point_ = transformation_matrix_.apply(end_point_);
    
    // 3. 检查是否与边界碰撞
    for (const auto& polygon : boundary_) {
        if (PolygonUtils::polygonCollidesWithLineSegment(polygon, start_point_, end_point_)) {
            return true;
        }
    }
    
    return false;
}
```

## 性能优化策略

### 1. 空间索引优化

使用 `LocToLineGrid` 进行空间分区：

```cpp
// 构建空间网格，加速碰撞检测
constexpr coord_t grid_size = 2000;  // 2mm网格
inside_loc_to_line_optimal_ = PolygonUtils::createLocToLineGrid(
    boundary_inside_optimal_, grid_size);
```

### 2. 距离阈值优化

```cpp
// 忽略短距离移动
const coord_t max_distance_ignored = 
    mesh_settings.get<coord_t>("machine_nozzle_tip_outer_diameter") / 2 * 2;

if (shorterThen(end_point - start_point, max_distance_ignored)) {
    return true;  // 直接成功，无需避障
}
```

### 3. 早期退出优化

```cpp
// 路径数量过多时使用简化策略
if (paths_.size() > 100) {
    return getDirectDistance(a, b) * 5;  // 静态惩罚因子
}
```

### 4. 缓存优化

```cpp
// 缓存避障网格
if (combing_grid_ == nullptr) {
    constexpr coord_t grid_size = 2000;
    combing_grid_ = PolygonUtils::createLocToLineGrid(*combing_boundary_, grid_size);
}
```

## 参数调优指南

### 1. retraction_combing 模式选择

- **OFF**: 禁用避障，适用于简单模型或高速打印
- **ALL**: 全面避障，适用于复杂模型，质量优先
- **NO_SKIN**: 避开表面，平衡质量和速度
- **INFILL**: 仅填充避障，适用于外观要求高的模型

### 2. retraction_combing_avoid_distance

```cpp
// 推荐值：0.1-0.5mm
// 较小值：更紧密的避障路径，可能增加计算时间
// 较大值：更安全的避障距离，可能增加移动距离
```

### 3. retraction_combing_max_distance

```cpp
// 推荐值：10-50mm
// 超过此距离的避障移动将强制回抽
// 平衡避障效果和移动效率
```

### 4. 相关参数协调

```cpp
// 参数组合建议
if (retraction_combing != OFF) {
    retraction_min_travel = 1.5;           // 减少短距离回抽
    retraction_hop_only_when_collides = true;  // 仅必要时Z跳跃
    travel_avoid_distance = 0.625;         // 适中的避让距离
}
```

## 特殊情况处理

### 1. 薄壁结构

```cpp
// 对于薄壁，可能需要调整避障策略
if (wall_thickness < 2 * nozzle_diameter) {
    // 使用更保守的避障模式
    combing_mode = CombingMode::INFILL;
}
```

### 2. 支撑结构

```cpp
// 支撑区域通常排除在避障边界外
if (mesh.settings.get<bool>("support_enable")) {
    comb_boundary = comb_boundary.difference(support_areas);
}
```

### 3. 桥接区域

```cpp
// 桥接区域需要特殊处理
if (is_bridge_layer) {
    // 可能需要禁用避障或使用特殊路径
    bypass_combing = true;
}
```

### 4. 多挤出头

```cpp
// 不同挤出头可能有不同的避障策略
for (size_t extruder_nr = 0; extruder_nr < extruder_count; ++extruder_nr) {
    const ExtruderTrain& train = scene.extruders[extruder_nr];
    // 根据挤出头特性调整避障参数
}
```

## 调试和诊断

### 1. 日志输出

```cpp
// 在关键位置添加日志
spdlog::debug("Combing calculation: start({}, {}), end({}, {}), mode={}", 
              start_point.X, start_point.Y, end_point.X, end_point.Y, 
              static_cast<int>(combing_mode));
```

### 2. 路径可视化

```cpp
// 输出避障路径用于调试
for (const auto& point : comb_path) {
    spdlog::debug("Comb point: ({}, {})", point.X, point.Y);
}
```

### 3. 性能监控

```cpp
// 监控避障计算时间
auto start_time = std::chrono::high_resolution_clock::now();
bool combed = comb_->calc(...);
auto end_time = std::chrono::high_resolution_clock::now();
auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
spdlog::debug("Combing calculation took {} microseconds", duration.count());
```

## 应用场景和最佳实践

### 1. 不同材料的优化

- **PLA**: 使用 ALL 模式，避障距离 0.2mm
- **ABS**: 使用 NO_SKIN 模式，避免表面缺陷
- **PETG**: 使用 INFILL 模式，减少拉丝
- **TPU**: 禁用避障，依赖回抽

### 2. 不同模型类型

- **机械零件**: NO_SKIN 模式，保证外观质量
- **艺术品**: ALL 模式，最大化避障效果
- **原型**: INFILL 模式，平衡速度和质量
- **功能件**: 根据具体要求选择模式

### 3. 打印速度优化

- **高速打印**: 使用 OFF 或 INFILL 模式
- **高质量打印**: 使用 ALL 或 NO_SKIN 模式
- **平衡模式**: 使用 NO_SKIN 模式

## 详细算法实现

### 1. LinePolygonsCrossings 核心算法

**文件位置**: `CuraEngine/src/pathPlanning/LinePolygonsCrossings.cpp`

#### 坐标变换算法

```cpp
bool LinePolygonsCrossings::lineSegmentCollidesWithBoundary()
{
    // 1. 计算线段方向向量
    Point2LL diff = end_point_ - start_point_;
    if (vSize2(diff) == 0) {
        return false;  // 起点终点相同
    }

    // 2. 创建变换矩阵，将线段对齐到x轴
    transformation_matrix_ = PointMatrix(diff);

    // 3. 变换起点和终点
    transformed_start_point_ = transformation_matrix_.apply(start_point_);
    transformed_end_point_ = transformation_matrix_.apply(end_point_);

    // 4. 确保变换后的y坐标相同（数值误差处理）
    transformed_end_point_.Y = transformed_start_point_.Y;

    // 5. 检查线段是否与任何边界多边形碰撞
    for (size_t poly_idx = 0; poly_idx < boundary_.size(); ++poly_idx) {
        const Polygon& polygon = boundary_[poly_idx];
        if (PolygonUtils::polygonCollidesWithLineSegment(polygon, start_point_, end_point_)) {
            return true;
        }
    }

    return false;
}
```

#### 扫描线交点计算

```cpp
bool LinePolygonsCrossings::calcScanlineCrossings(bool fail_on_unavoidable_obstacles)
{
    crossings_.clear();

    const coord_t y = transformed_start_point_.Y;  // 扫描线的y坐标
    const coord_t x_min = std::min(transformed_start_point_.X, transformed_end_point_.X);
    const coord_t x_max = std::max(transformed_start_point_.X, transformed_end_point_.X);

    // 遍历所有边界多边形
    for (size_t poly_idx = 0; poly_idx < boundary_.size(); ++poly_idx) {
        const Polygon& polygon = boundary_[poly_idx];

        // 检查多边形的每条边
        for (size_t point_idx = 0; point_idx < polygon.size(); ++point_idx) {
            Point2LL p0 = transformation_matrix_.apply(polygon[point_idx]);
            Point2LL p1 = transformation_matrix_.apply(polygon[(point_idx + 1) % polygon.size()]);

            // 检查边是否与扫描线相交
            if ((p0.Y <= y && p1.Y > y) || (p1.Y <= y && p0.Y > y)) {
                // 计算交点的x坐标
                coord_t x_crossing = p0.X + (p1.X - p0.X) * (y - p0.Y) / (p1.Y - p0.Y);

                // 检查交点是否在线段范围内
                if (x_crossing >= x_min && x_crossing <= x_max) {
                    crossings_.emplace_back(poly_idx, x_crossing, point_idx);
                }
            }
        }
    }

    // 按x坐标排序交点
    std::sort(crossings_.begin(), crossings_.end(),
              [](const Crossing& a, const Crossing& b) {
                  return a.x_ < b.x_;
              });

    // 检查是否有不可避免的障碍
    if (fail_on_unavoidable_obstacles && crossings_.size() % 2 != 0) {
        return false;  // 奇数个交点表示有不可避免的障碍
    }

    return true;
}
```

#### 基础避障路径生成

```cpp
void LinePolygonsCrossings::generateBasicCombingPath(CombPath& combPath)
{
    combPath.clear();

    if (crossings_.empty()) {
        return;  // 无交点，直线路径
    }

    Point2LL current_point = start_point_;

    // 处理成对的交点
    for (size_t i = 0; i < crossings_.size(); i += 2) {
        if (i + 1 >= crossings_.size()) {
            break;  // 避免越界
        }

        const Crossing& entry_crossing = crossings_[i];
        const Crossing& exit_crossing = crossings_[i + 1];

        // 移动到入口点
        Point2LL entry_point = transformation_matrix_.unapply(
            Point2LL(entry_crossing.x_, transformed_start_point_.Y));
        combPath.push_back(entry_point);

        // 沿着多边形边界移动
        generateBasicCombingPath(entry_crossing, exit_crossing, combPath);

        // 移动到出口点
        Point2LL exit_point = transformation_matrix_.unapply(
            Point2LL(exit_crossing.x_, transformed_start_point_.Y));
        combPath.push_back(exit_point);

        current_point = exit_point;
    }
}
```

#### 沿边界路径生成

```cpp
void LinePolygonsCrossings::generateBasicCombingPath(const Crossing& min, const Crossing& max,
                                                    CombPath& combPath)
{
    if (min.poly_idx_ != max.poly_idx_) {
        return;  // 不同多边形，无法沿边界移动
    }

    const Polygon& polygon = boundary_[min.poly_idx_];

    // 确定移动方向（顺时针或逆时针）
    bool clockwise = shouldMoveClockwise(min, max, polygon);

    size_t current_point_idx = min.point_idx_;
    size_t target_point_idx = max.point_idx_;

    // 沿着多边形边界添加点
    while (current_point_idx != target_point_idx) {
        // 移动到下一个点
        if (clockwise) {
            current_point_idx = (current_point_idx + 1) % polygon.size();
        } else {
            current_point_idx = (current_point_idx + polygon.size() - 1) % polygon.size();
        }

        // 添加偏移后的点
        Point2LL boundary_point = polygon[current_point_idx];
        Point2LL offset_point = movePointAwayFromBoundary(boundary_point, polygon);
        combPath.push_back(offset_point);
    }
}
```

#### 路径优化算法

```cpp
bool LinePolygonsCrossings::optimizePath(CombPath& comb_path, CombPath& optimized_comb_path)
{
    optimized_comb_path.clear();

    if (comb_path.size() <= 2) {
        optimized_comb_path = comb_path;
        return false;
    }

    optimized_comb_path.push_back(comb_path.front());

    size_t current_index = 0;
    bool path_crosses_boundary = false;

    while (current_index < comb_path.size() - 1) {
        size_t next_index = current_index + 1;

        // 尝试跳过中间点
        while (next_index < comb_path.size() - 1) {
            Point2LL current_point = comb_path[current_index];
            Point2LL candidate_point = comb_path[next_index + 1];

            // 检查直线路径是否与边界碰撞
            bool collides = false;
            for (const Polygon& polygon : boundary_) {
                if (PolygonUtils::polygonCollidesWithLineSegment(polygon, current_point, candidate_point)) {
                    collides = true;
                    break;
                }
            }

            if (collides) {
                break;  // 无法跳过更多点
            }

            next_index++;
        }

        // 添加优化后的点
        if (next_index < comb_path.size()) {
            optimized_comb_path.push_back(comb_path[next_index]);

            // 检查是否跳过了边界交点
            if (next_index > current_index + 1) {
                path_crosses_boundary = true;
            }
        }

        current_index = next_index;
    }

    return path_crosses_boundary;
}
```

### 2. Comb 类的高级功能

#### 内部到内部避障

```cpp
bool Comb::calcInsideToInside(Point2LL start_point, Point2LL end_point,
                             CombPaths& comb_paths, coord_t max_comb_distance_ignored)
{
    // 1. 检查是否可以直接连接
    if (!PolygonUtils::polygonCollidesWithLineSegment(boundary_inside_optimal_,
                                                     start_point, end_point)) {
        return true;  // 直接路径可行
    }

    // 2. 尝试使用最优边界进行避障
    CombPath result_path;
    bool comb_result = LinePolygonsCrossings::comb(
        boundary_inside_optimal_,
        *inside_loc_to_line_optimal_,
        start_point,
        end_point,
        result_path,
        -offset_dist_to_get_from_on_the_polygon_to_outside_,
        max_comb_distance_ignored,
        false);  // 不在不可避免障碍时失败

    if (comb_result) {
        comb_paths.emplace_back();
        moveCombPathInside(boundary_inside_minimum_, boundary_inside_optimal_,
                          result_path, comb_paths.back());
        return true;
    }

    // 3. 回退到最小边界
    comb_result = LinePolygonsCrossings::comb(
        boundary_inside_minimum_,
        *inside_loc_to_line_minimum_,
        start_point,
        end_point,
        result_path,
        -offset_dist_to_get_from_on_the_polygon_to_outside_,
        max_comb_distance_ignored,
        false);

    if (comb_result) {
        comb_paths.emplace_back();
        moveCombPathInside(boundary_inside_minimum_, boundary_inside_optimal_,
                          result_path, comb_paths.back());
        return true;
    }

    return false;
}
```

#### 路径内移算法

```cpp
void Comb::moveCombPathInside(const Shape& boundary_inside_minimum,
                             const Shape& boundary_inside_optimal,
                             CombPath& comb_path_input, CombPath& comb_path_output)
{
    comb_path_output.clear();

    for (const Point2LL& point : comb_path_input) {
        Point2LL moved_point = point;

        // 检查点是否在最小边界内
        if (!boundary_inside_minimum.inside(point, true)) {
            // 将点移动到边界内
            ClosestPointPolygon closest_point = PolygonUtils::findClosest(point, boundary_inside_minimum);

            if (closest_point.isValid()) {
                // 计算移动方向
                Point2LL direction = point - closest_point.p();
                coord_t distance = vSize(direction);

                if (distance > 0) {
                    // 向内移动指定距离
                    Point2LL normalized_direction = direction * offset_from_outlines_ / distance;
                    moved_point = closest_point.p() + normalized_direction;
                }
            }
        }

        // 确保点在最优边界内
        if (!boundary_inside_optimal.inside(moved_point, true)) {
            ClosestPointPolygon closest_optimal = PolygonUtils::findClosest(moved_point, boundary_inside_optimal);
            if (closest_optimal.isValid()) {
                moved_point = closest_optimal.p();
            }
        }

        comb_path_output.push_back(moved_point);
    }
}
```

### 3. 高级避障策略

#### 多部件避障

```cpp
bool Comb::calcThroughMultipleParts(Point2LL start_point, Point2LL end_point,
                                   CombPaths& comb_paths)
{
    // 1. 识别起点和终点所在的部件
    int start_part_idx = findPartContaining(start_point);
    int end_part_idx = findPartContaining(end_point);

    if (start_part_idx == end_part_idx && start_part_idx >= 0) {
        // 同一部件内，使用标准避障
        return calcInsideToInside(start_point, end_point, comb_paths, 0);
    }

    // 2. 跨部件移动，需要特殊处理
    comb_paths.throughAir = true;

    // 找到最佳的中间连接点
    std::vector<Point2LL> connection_points = findConnectionPoints(start_part_idx, end_part_idx);

    if (connection_points.empty()) {
        return false;  // 无法找到连接路径
    }

    // 3. 分段计算避障路径
    Point2LL current_point = start_point;

    for (const Point2LL& connection_point : connection_points) {
        CombPaths segment_paths;

        if (calcInsideToInside(current_point, connection_point, segment_paths, 0)) {
            comb_paths.insert(comb_paths.end(), segment_paths.begin(), segment_paths.end());
        } else {
            // 直接连接
            CombPath direct_path;
            direct_path.push_back(current_point);
            direct_path.push_back(connection_point);
            direct_path.cross_boundary = true;
            comb_paths.push_back(direct_path);
        }

        current_point = connection_point;
    }

    // 4. 最后一段到终点
    CombPaths final_segment;
    if (calcInsideToInside(current_point, end_point, final_segment, 0)) {
        comb_paths.insert(comb_paths.end(), final_segment.begin(), final_segment.end());
    } else {
        CombPath direct_path;
        direct_path.push_back(current_point);
        direct_path.push_back(end_point);
        direct_path.cross_boundary = true;
        comb_paths.push_back(direct_path);
    }

    return true;
}
```

#### Z跳跃决策算法

```cpp
bool Comb::shouldPerformZHop(const CombPaths& comb_paths,
                            bool perform_z_hops,
                            bool perform_z_hops_only_when_collides,
                            coord_t travel_distance)
{
    if (!perform_z_hops) {
        return false;
    }

    // 1. 检查是否总是Z跳跃
    if (!perform_z_hops_only_when_collides) {
        return true;
    }

    // 2. 检查路径是否穿越边界
    for (const CombPath& path : comb_paths) {
        if (path.cross_boundary) {
            return true;
        }
    }

    // 3. 检查是否通过空气移动
    if (comb_paths.throughAir) {
        return true;
    }

    // 4. 检查移动距离
    const coord_t z_hop_threshold = MM2INT(5.0);  // 5mm阈值
    if (travel_distance > z_hop_threshold) {
        return true;
    }

    return false;
}
```

### 4. 性能优化实现

#### 空间索引优化

```cpp
std::unique_ptr<LocToLineGrid> Comb::createOptimizedGrid(const Shape& boundary,
                                                        coord_t grid_size)
{
    // 1. 计算边界框
    AABB boundary_box(boundary);

    // 2. 动态调整网格大小
    coord_t optimal_grid_size = std::max(grid_size,
                                        std::min(boundary_box.max_.X - boundary_box.min_.X,
                                               boundary_box.max_.Y - boundary_box.min_.Y) / 100);

    // 3. 创建网格
    auto grid = PolygonUtils::createLocToLineGrid(boundary, optimal_grid_size);

    // 4. 预计算常用查询
    precomputeGridQueries(grid.get(), boundary_box);

    return grid;
}

void Comb::precomputeGridQueries(LocToLineGrid* grid, const AABB& boundary_box)
{
    // 预计算网格中心点的查询结果
    const coord_t step = MM2INT(1.0);  // 1mm步长

    for (coord_t x = boundary_box.min_.X; x <= boundary_box.max_.X; x += step) {
        for (coord_t y = boundary_box.min_.Y; y <= boundary_box.max_.Y; y += step) {
            Point2LL query_point(x, y);
            grid->getNearby(query_point, step / 2);  // 预热缓存
        }
    }
}
```

#### 路径缓存机制

```cpp
class CombPathCache
{
private:
    struct CacheKey
    {
        Point2LL start, end;
        size_t boundary_hash;

        bool operator==(const CacheKey& other) const {
            return start == other.start && end == other.end &&
                   boundary_hash == other.boundary_hash;
        }
    };

    struct CacheKeyHash
    {
        size_t operator()(const CacheKey& key) const {
            return std::hash<coord_t>()(key.start.X) ^
                   std::hash<coord_t>()(key.start.Y) ^
                   std::hash<coord_t>()(key.end.X) ^
                   std::hash<coord_t>()(key.end.Y) ^
                   key.boundary_hash;
        }
    };

    std::unordered_map<CacheKey, CombPaths, CacheKeyHash> cache_;
    size_t max_cache_size_ = 1000;

public:
    bool getCachedPath(Point2LL start, Point2LL end, size_t boundary_hash,
                      CombPaths& result)
    {
        CacheKey key{start, end, boundary_hash};
        auto it = cache_.find(key);

        if (it != cache_.end()) {
            result = it->second;
            return true;
        }

        return false;
    }

    void cachePath(Point2LL start, Point2LL end, size_t boundary_hash,
                  const CombPaths& paths)
    {
        if (cache_.size() >= max_cache_size_) {
            // 简单的LRU：清除一半缓存
            auto it = cache_.begin();
            std::advance(it, cache_.size() / 2);
            cache_.erase(cache_.begin(), it);
        }

        CacheKey key{start, end, boundary_hash};
        cache_[key] = paths;
    }
};
```

Retraction Combing 系统通过复杂的几何算法和智能路径规划，显著减少了不必要的回抽操作，提高了打印质量和效率。这些详细的实现展示了系统的复杂性和精密性，正确理解和配置这个系统对于获得最佳打印效果至关重要。
