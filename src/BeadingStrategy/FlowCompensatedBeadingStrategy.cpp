// Copyright (c) 2024 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#include "BeadingStrategy/FlowCompensatedBeadingStrategy.h"

#include <algorithm>
#include <spdlog/spdlog.h>

#include "utils/macros.h"
#include "utils/DebugManager.h"

namespace cura
{

FlowCompensatedBeadingStrategy::FlowCompensatedBeadingStrategy(
    BeadingStrategyPtr parent, 
    const coord_t min_target_width,
    const coord_t min_stable_width,
    const double max_flow_compensation_ratio)
    : BeadingStrategy(*parent)
    , parent_(std::move(parent))
    , min_target_width_(min_target_width)
    , min_stable_width_(min_stable_width)
    , max_flow_compensation_ratio_(max_flow_compensation_ratio)
{
    name_ = "FlowCompensatedBeadingStrategy";
    
    // 参数验证
    if (min_stable_width_ < min_target_width_) {
        CURA_WARN("FlowCompensatedBeadingStrategy: min_stable_width ({:.2f}mm) < min_target_width ({:.2f}mm)，将调整为相等", 
                     INT2MM(min_stable_width_), INT2MM(min_target_width_));
        const_cast<coord_t&>(min_stable_width_) = min_target_width_;
    }
    
    CURA_DEBUG(FLOW_COMPENSATION, "FlowCompensatedBeadingStrategy初始化: 目标最小宽度={:.2f}mm, 稳定最小宽度={:.2f}mm, 最大补偿比例={:.1f}%", 
                  INT2MM(min_target_width_), INT2MM(min_stable_width_), max_flow_compensation_ratio_ * 100);
}

std::string FlowCompensatedBeadingStrategy::toString() const
{
    return std::string("FlowCompensated+") + parent_->toString();
}

bool FlowCompensatedBeadingStrategy::needsFlowCompensation(coord_t thickness) const
{
    return thickness < min_stable_width_ && thickness >= min_target_width_;
}

double FlowCompensatedBeadingStrategy::calculateFlowRatio(coord_t target_width, coord_t stable_width) const
{
    if (stable_width <= 0) {
        return 1.0;
    }
    
    const double raw_ratio = static_cast<double>(target_width) / static_cast<double>(stable_width);
    
    // 限制在合理范围内
    const double min_ratio = max_flow_compensation_ratio_;
    const double max_ratio = 1.0;
    
    return std::max(min_ratio, std::min(max_ratio, raw_ratio));
}

FlowCompensatedBeadingStrategy::Beading FlowCompensatedBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    // 如果厚度太小，无法处理
    if (thickness < min_target_width_) {
        Beading ret;
        ret.total_thickness = thickness;
        ret.left_over = thickness;
        return ret;
    }
    
    // 如果不需要流量补偿，直接使用父策略
    if (!needsFlowCompensation(thickness)) {
        return parent_->compute(thickness, bead_count);
    }
    
    // 需要流量补偿：使用稳定宽度进行计算，然后调整flow
    CURA_DEBUG(FLOW_COMPENSATION, "厚度{:.2f}mm需要流量补偿", INT2MM(thickness));
    
    // 使用稳定宽度计算beading
    Beading stable_beading = parent_->compute(min_stable_width_, bead_count);
    
    // 应用流量补偿
    return applyFlowCompensation(stable_beading, thickness);
}

FlowCompensatedBeadingStrategy::Beading FlowCompensatedBeadingStrategy::applyFlowCompensation(const Beading& beading, coord_t target_thickness) const
{
    Beading result = beading;
    result.total_thickness = target_thickness;
    
    if (beading.bead_widths.empty()) {
        result.left_over = target_thickness;
        return result;
    }
    
    // 计算总的稳定宽度
    coord_t total_stable_width = 0;
    for (coord_t width : beading.bead_widths) {
        total_stable_width += width;
    }
    
    if (total_stable_width <= 0) {
        result.left_over = target_thickness;
        return result;
    }
    
    // 计算流量补偿比例
    const double flow_ratio = calculateFlowRatio(target_thickness, total_stable_width);
    
    // 调整每个bead的宽度（保持比例）并设置flow_ratios
    coord_t adjusted_total_width = 0;
    result.flow_ratios.resize(result.bead_widths.size());
    
    for (size_t i = 0; i < result.bead_widths.size(); i++) {
        const coord_t original_width = beading.bead_widths[i];
        const coord_t adjusted_width = static_cast<coord_t>(original_width * flow_ratio);
        result.bead_widths[i] = std::max(static_cast<coord_t>(1), adjusted_width); // 至少1微米
        result.flow_ratios[i] = flow_ratio; // 设置流量比例
        adjusted_total_width += result.bead_widths[i];
    }
    
    // 计算剩余厚度
    result.left_over = target_thickness - adjusted_total_width;
    if (result.left_over < 0) {
        result.left_over = 0;
    }
    
    CURA_DEBUG(FLOW_COMPENSATION, "流量补偿 {:.2f}mm -> {:.2f}mm (比例={:.3f})", 
                  INT2MM(total_stable_width), INT2MM(adjusted_total_width), flow_ratio);
    
    return result;
}

coord_t FlowCompensatedBeadingStrategy::getOptimalThickness(coord_t bead_count) const
{
    return parent_->getOptimalThickness(bead_count);
}

coord_t FlowCompensatedBeadingStrategy::getTransitionThickness(coord_t lower_bead_count) const
{
    coord_t parent_transition = parent_->getTransitionThickness(lower_bead_count);
    
    // 如果父策略的过渡厚度小于我们的稳定宽度，使用稳定宽度
    if (parent_transition < min_stable_width_) {
        return min_stable_width_;
    }
    
    return parent_transition;
}

coord_t FlowCompensatedBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    // 对于极小厚度，返回1（如果大于等于目标最小宽度）
    if (thickness >= min_target_width_ && thickness < min_stable_width_) {
        return 1;
    }
    
    return parent_->getOptimalBeadCount(thickness);
}

coord_t FlowCompensatedBeadingStrategy::getTransitioningLength(coord_t lower_bead_count) const
{
    return parent_->getTransitioningLength(lower_bead_count);
}

double FlowCompensatedBeadingStrategy::getTransitionAnchorPos(coord_t lower_bead_count) const
{
    return parent_->getTransitionAnchorPos(lower_bead_count);
}

std::vector<coord_t> FlowCompensatedBeadingStrategy::getNonlinearThicknesses(coord_t lower_bead_count) const
{
    std::vector<coord_t> result = parent_->getNonlinearThicknesses(lower_bead_count);
    
    // 添加我们的关键厚度点
    result.emplace_back(min_target_width_);
    result.emplace_back(min_stable_width_);
    
    // 排序并去重
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    
    return result;
}

} // namespace cura
