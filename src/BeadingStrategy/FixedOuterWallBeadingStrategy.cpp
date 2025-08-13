// Copyright (c) 2025 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#include "BeadingStrategy/FixedOuterWallBeadingStrategy.h"
#include "utils/DebugManager.h"

#include <algorithm>
#include <numeric>

namespace cura
{

FixedOuterWallBeadingStrategy::FixedOuterWallBeadingStrategy(const coord_t fixed_outer_width, const Ratio minimum_variable_line_ratio, BeadingStrategyPtr parent)
    : BeadingStrategy(*parent)
    , parent_(std::move(parent))
    , fixed_outer_width_(fixed_outer_width)
    , minimum_variable_line_ratio_(minimum_variable_line_ratio)
{
    name_ = "FixedOuterWallBeadingStrategy";
}

coord_t FixedOuterWallBeadingStrategy::getOptimalThickness(coord_t bead_count) const
{
    if (bead_count <= 0) return 0;
    if (bead_count == 1) return fixed_outer_width_;
    if (bead_count == 2) return 2 * fixed_outer_width_;
    
    // For 3+ beads: 2 fixed outer walls + inner walls from parent strategy
    const coord_t inner_bead_count = bead_count - 2;
    return 2 * fixed_outer_width_ + parent_->getOptimalThickness(inner_bead_count);
}

coord_t FixedOuterWallBeadingStrategy::getTransitionThickness(coord_t lower_bead_count) const
{
    switch (lower_bead_count)
    {
    case 0:
        return minimum_variable_line_ratio_ * fixed_outer_width_;
    case 1:
        return fixed_outer_width_ + minimum_variable_line_ratio_ * fixed_outer_width_;
    case 2:
        return 2 * fixed_outer_width_ + minimum_variable_line_ratio_ * parent_->getOptimalWidth();
    default:
        return 2 * fixed_outer_width_ + parent_->getTransitionThickness(lower_bead_count - 2);
    }
}

coord_t FixedOuterWallBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    if (thickness < minimum_variable_line_ratio_ * fixed_outer_width_)
    {
        return 0;
    }
    if (thickness <= fixed_outer_width_)
    {
        return 1;
    }
    if (thickness <= 2 * fixed_outer_width_)
    {
        return 2;
    }
    
    // For thickness > 2 * fixed_outer_width_, calculate inner walls
    const coord_t inner_thickness = thickness - 2 * fixed_outer_width_;
    return 2 + parent_->getOptimalBeadCount(inner_thickness);
}

coord_t FixedOuterWallBeadingStrategy::getTransitioningLength(coord_t lower_bead_count) const
{
    return parent_->getTransitioningLength(lower_bead_count);
}

double FixedOuterWallBeadingStrategy::getTransitionAnchorPos(coord_t lower_bead_count) const
{
    return parent_->getTransitionAnchorPos(lower_bead_count);
}

std::string FixedOuterWallBeadingStrategy::toString() const
{
    return std::string("FixedOuterWall+") + parent_->toString();
}

BeadingStrategy::Beading FixedOuterWallBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    Beading ret;
    ret.total_thickness = thickness;

    CURA_DEBUG(BEADING_STRATEGY, "FixedOuterWall计算：厚度={:.3f}mm, 目标线数={}, 固定外墙宽度={:.3f}mm", 
               INT2MM(thickness), bead_count, INT2MM(fixed_outer_width_));

    // Handle cases with no lines
    if (bead_count == 0 || thickness < minimum_variable_line_ratio_ * fixed_outer_width_)
    {
        ret.left_over = thickness;
        CURA_DEBUG(BEADING_STRATEGY, "厚度太小或无线数，全部作为left_over");
        return ret;
    }

    // Single wall case
    if (bead_count == 1)
    {
        ret.bead_widths.push_back(fixed_outer_width_);
        ret.toolpath_locations.push_back(thickness / 2);
        ret.left_over = thickness - fixed_outer_width_;
        CURA_DEBUG(BEADING_STRATEGY, "单墙：固定宽度={:.3f}mm, left_over={:.3f}mm", 
                   INT2MM(fixed_outer_width_), INT2MM(ret.left_over));
        return ret;
    }

    // Two walls case
    if (bead_count == 2)
    {
        ret.bead_widths.push_back(fixed_outer_width_);
        ret.bead_widths.push_back(fixed_outer_width_);
        ret.toolpath_locations.push_back(fixed_outer_width_ / 2);
        ret.toolpath_locations.push_back(thickness - fixed_outer_width_ / 2);
        ret.left_over = thickness - 2 * fixed_outer_width_;
        CURA_DEBUG(BEADING_STRATEGY, "双墙：两个固定宽度={:.3f}mm, left_over={:.3f}mm", 
                   INT2MM(fixed_outer_width_), INT2MM(ret.left_over));
        return ret;
    }

    // Three or more walls: 2 fixed outer + inner walls from parent strategy
    const coord_t inner_bead_count = bead_count - 2;
    const coord_t inner_thickness = thickness - 2 * fixed_outer_width_;
    
    CURA_DEBUG(BEADING_STRATEGY, "多墙模式：内墙数={}, 内墙厚度={:.3f}mm", inner_bead_count, INT2MM(inner_thickness));

    if (inner_thickness > 0 && inner_bead_count > 0)
    {
        // Compute inner walls using parent strategy
        Beading inner_beading = parent_->compute(inner_thickness, inner_bead_count);
        
        // Adjust inner wall positions (shift by fixed_outer_width_)
        for (auto& location : inner_beading.toolpath_locations)
        {
            location += fixed_outer_width_;
        }
        
        // Copy inner walls to result
        ret.bead_widths = inner_beading.bead_widths;
        ret.toolpath_locations = inner_beading.toolpath_locations;
        ret.left_over = inner_beading.left_over;
    }

    // Add fixed outer walls
    ret.bead_widths.insert(ret.bead_widths.begin(), fixed_outer_width_);  // First outer wall
    ret.toolpath_locations.insert(ret.toolpath_locations.begin(), fixed_outer_width_ / 2);
    
    ret.bead_widths.push_back(fixed_outer_width_);  // Last outer wall
    ret.toolpath_locations.push_back(thickness - fixed_outer_width_ / 2);

    CURA_DEBUG(BEADING_STRATEGY, "最终结果：总线数={}, left_over={:.3f}mm", ret.bead_widths.size(), INT2MM(ret.left_over));

    return ret;
}

} // namespace cura
