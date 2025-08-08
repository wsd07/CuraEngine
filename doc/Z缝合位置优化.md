# Z缝合位置优化

## 概述

Z缝合位置优化是 CuraEngine 路径优化系统的重要组成部分，负责确定每个闭合路径的起始点位置，以优化打印质量和外观效果。

## 核心算法

### 缝合点查找算法 (findStartLocationWithZ)

这是一个复杂的多标准评分系统：

```cpp
size_t findStartLocationWithZ(const OrderablePath& path, const Point2LL& target_pos, coord_t layer_z)
{
    if (!path.is_closed_) {
        // 开放路径：选择距离目标点最近的端点
        const coord_t back_distance = (combing_boundary_ == nullptr) ? 
            getDirectDistance(path.converted_->back(), target_pos) : 
            getCombingDistance(path.converted_->back(), target_pos);
        
        if (back_distance < getDirectDistance(path.converted_->front(), target_pos)) {
            return path.converted_->size() - 1;
        }
        return 0;
    }
    
    // 闭合路径：使用多标准评分系统
    BestElementFinder best_candidate_finder;
    BestElementFinder::CriteriaPass main_criteria_pass;
    main_criteria_pass.outsider_delta_threshold = 0.05;
    
    // 主要评分标准
    BestElementFinder::WeighedCriterion main_criterion;
    
    // 自定义Z缝合点处理
    if (path.seam_config_.draw_z_seam_enable_) {
        auto interpolated_pos = path.seam_config_.getInterpolatedSeamPosition();
        if (interpolated_pos.has_value()) {
            Point2LL custom_target_pos = interpolated_pos.value();
            main_criterion.criterion = std::make_shared<DistanceScoringCriterion>(points, custom_target_pos);
        }
    }
    // 强制起始点
    else if (path.force_start_index_.has_value()) {
        main_criterion.criterion = std::make_shared<DistanceScoringCriterion>(
            points, points.at(path.force_start_index_.value()), 
            DistanceScoringCriterion::DistanceType::Euclidian, 1.0);
    }
    // 最短距离或用户指定
    else if (path.seam_config_.type_ == EZSeamType::SHORTEST || 
             path.seam_config_.type_ == EZSeamType::USER_SPECIFIED) {
        main_criterion.criterion = std::make_shared<DistanceScoringCriterion>(points, target_pos);
    }
    // 最尖锐角
    else if (path.seam_config_.type_ == EZSeamType::SHARPEST_CORNER) {
        main_criterion.criterion = std::make_shared<CornerScoringCriterion>(points, path.seam_config_.corner_pref_);
    }
    // 随机
    else if (path.seam_config_.type_ == EZSeamType::RANDOM) {
        main_criterion.criterion = std::make_shared<RandomScoringCriterion>();
    }
    
    // 添加回退策略
    if (path.seam_config_.type_ == EZSeamType::SHARPEST_CORNER) {
        // 回退策略1：选择最后方的点
        auto fallback_criterion = std::make_shared<DistanceScoringCriterion>(
            points, path_bounding_box.max_, DistanceScoringCriterion::DistanceType::YOnly);
        best_candidate_finder.appendSingleCriterionPass(fallback_criterion, 0.01);
        
        // 回退策略2：选择最右方的点
        auto fallback_criterion2 = std::make_shared<DistanceScoringCriterion>(
            points, path_bounding_box.max_, DistanceScoringCriterion::DistanceType::XOnly);
        best_candidate_finder.appendSingleCriterionPass(fallback_criterion2);
    }
    
    // 应用评分标准
    std::optional<size_t> best_i = best_candidate_finder.findBestElement(points.size());
    
    // 检查禁止区域
    if (!disallowed_area_for_seams.empty()) {
        best_i = pathIfZseamIsInDisallowedArea(best_i.value_or(0), path, 0);
    }
    
    return best_i.value_or(0);
}
```

## 评分标准系统

### BestElementFinder 类

**文件位置**: `CuraEngine/include/utils/scoring/BestElementFinder.h`

这是一个通用的多标准评分框架：

```cpp
class BestElementFinder
{
public:
    struct WeighedCriterion
    {
        std::shared_ptr<ScoringCriterion> criterion;  // 评分标准
        double weight = 1.0;                          // 权重
    };

    struct CriteriaPass
    {
        std::vector<WeighedCriterion> criteria;       // 评分标准列表
        double outsider_delta_threshold = 0.0;       // 异常值阈值
    };

    void appendCriteriaPass(const CriteriaPass& pass);
    void appendSingleCriterionPass(std::shared_ptr<ScoringCriterion> criterion,
                                  double outsider_delta_threshold = 0.0);
    std::optional<size_t> findBestElement(size_t element_count);
};
```

### 具体评分标准

#### DistanceScoringCriterion（距离评分）

```cpp
class DistanceScoringCriterion : public ScoringCriterion
{
public:
    enum class DistanceType
    {
        Euclidian,    // 欧几里得距离
        XOnly,        // 仅X方向距离
        YOnly,        // 仅Y方向距离
    };

    DistanceScoringCriterion(const PointsSet& points,
                           const Point2LL& target_point,
                           DistanceType distance_type = DistanceType::Euclidian,
                           double distance_divider = 1000.0);

    double computeScore(size_t candidate_index) const override;
};
```

#### CornerScoringCriterion（角度评分）

```cpp
class CornerScoringCriterion : public ScoringCriterion
{
public:
    CornerScoringCriterion(const PointsSet& points,
                          EZSeamCornerPrefType corner_preference);

    double computeScore(size_t candidate_index) const override;

private:
    double calculateCornerAngle(size_t index) const;
    double getCornerScore(double angle) const;
};
```

#### RandomScoringCriterion（随机评分）

```cpp
class RandomScoringCriterion : public ScoringCriterion
{
public:
    RandomScoringCriterion(uint32_t seed = 0);
    double computeScore(size_t candidate_index) const override;

private:
    mutable std::mt19937 random_generator_;
};
```

## 高级优化技术

### 自定义Z缝合点系统

**实现位置**: `PathOrderOptimizer::findStartLocationWithZ` 函数

```cpp
// 自定义Z缝合点处理逻辑
if (path.seam_config_.draw_z_seam_enable_) {
    spdlog::info("=== 外轮廓自定义Z接缝点处理开始 ===");

    // 尝试获取当前层的插值接缝位置
    auto interpolated_pos = path.seam_config_.getInterpolatedSeamPosition();
    if (interpolated_pos.has_value()) {
        Point2LL custom_target_pos = interpolated_pos.value();

        // 创建距离评分标准，优先选择最接近自定义位置的顶点
        main_criterion.criterion = std::make_shared<DistanceScoringCriterion>(
            points, custom_target_pos);

        spdlog::info("使用自定义接缝位置: ({:.2f}, {:.2f})",
                   INT2MM(custom_target_pos.X), INT2MM(custom_target_pos.Y));
    } else {
        // 插值失败，回退到默认处理方式
        spdlog::info("插值失败，使用默认处理方式");
        main_criterion.criterion = std::make_shared<DistanceScoringCriterion>(
            points, target_pos);
    }
}
```

### 禁止区域处理

```cpp
size_t pathIfZseamIsInDisallowedArea(size_t best_pos, const OrderablePath& path,
                                   size_t number_of_paths_analysed)
{
    size_t path_size = path.converted_->size();
    if (path_size > number_of_paths_analysed) {
        if (!disallowed_area_for_seams.empty()) {
            Point2LL current_candidate = (path.converted_)->at(best_pos);
            if (disallowed_area_for_seams.inside(current_candidate, true)) {
                // 当前位置在禁止区域内，寻找下一个位置
                size_t next_best_position = (path_size > best_pos + 1) ? best_pos + 1 : 0;
                number_of_paths_analysed += 1;
                best_pos = pathIfZseamIsInDisallowedArea(next_best_position, path,
                                                       number_of_paths_analysed);
            }
        }
    } else {
        spdlog::warn("No start path found for support z seam distance");
        // 所有位置都在禁止区域内，可能需要计算最佳妥协位置
    }
    return best_pos;
}
```

### 闭合回路检测

```cpp
bool isLoopingPolyline(const OrderablePath& path)
{
    if (path.converted_->empty()) {
        return false;
    }

    // 检查首尾点距离是否小于重合阈值
    coord_t distance_squared = vSize2(path.converted_->back() - path.converted_->front());
    return distance_squared < _coincident_point_distance * _coincident_point_distance;
}
```

## 应用场景

### 不同打印类型的缝合策略

- **外壁 (Outer Walls)**: 重点优化缝合位置和表面质量
- **内壁 (Inner Walls)**: 优先考虑打印效率
- **填充 (Infill)**: 最小化移动距离
- **表面 (Skin)**: 平衡质量和效率
- **支撑 (Support)**: 优化移除便利性

### 特殊处理

- **悬垂区域**: 避免在悬垂处开始打印
- **桥接**: 优化桥接方向和起始点
- **薄壁**: 特殊的缝合策略
- **小特征**: 减速和特殊路径规划

## 缝合质量分析

```cpp
void analyzeSeamQuality(const std::vector<OrderablePath>& paths)
{
    std::map<EZSeamType, int> seam_type_count;
    std::vector<double> seam_distances;

    for (const auto& path : paths) {
        seam_type_count[path.seam_config_.type_]++;

        if (path.is_closed_ && !path.converted_->empty()) {
            Point2LL seam_pos = (*path.converted_)[path.start_vertex_];
            // 分析缝合位置质量...
        }
    }

    spdlog::info("Seam analysis: SHORTEST={}, SHARPEST_CORNER={}, RANDOM={}",
                seam_type_count[EZSeamType::SHORTEST],
                seam_type_count[EZSeamType::SHARPEST_CORNER],
                seam_type_count[EZSeamType::RANDOM]);
}
```
