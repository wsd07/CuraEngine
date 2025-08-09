// Copyright (c) 2022 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#include "BeadingStrategy/WideningBeadingStrategy.h"

#include <spdlog/spdlog.h>

#include "utils/macros.h"

namespace cura
{

WideningBeadingStrategy::WideningBeadingStrategy(BeadingStrategyPtr parent, const coord_t min_input_width, const coord_t min_output_width)
    : BeadingStrategy(*parent)
    , parent_(std::move(parent))
    , min_input_width_(min_input_width)
    , min_output_width_(min_output_width)
{
    // 安全检查：确保参数合理
    if (min_output_width_ < optimal_width_ / 4) {
        spdlog::warn("WideningBeadingStrategy: min_output_width ({:.2f}mm) 过小，可能导致计算错误",
                     INT2MM(min_output_width_));
    }
    if (min_input_width_ < min_output_width_) {
        spdlog::warn("WideningBeadingStrategy: min_input_width ({:.2f}mm) < min_output_width ({:.2f}mm)，逻辑可能有问题",
                     INT2MM(min_input_width_), INT2MM(min_output_width_));
    }
}

std::string WideningBeadingStrategy::toString() const
{
    return std::string("Widening+") + parent_->toString();
}

WideningBeadingStrategy::Beading WideningBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    if (thickness < optimal_width_)
    {
        Beading ret;
        ret.total_thickness = thickness;
        if (thickness >= min_input_width_)
        {
            // 安全计算：确保输出宽度不会过大
            const coord_t safe_output_width = std::min(
                std::max(thickness, min_output_width_),
                optimal_width_  // 不超过最优宽度
            );

            ret.bead_widths.emplace_back(safe_output_width);
            ret.toolpath_locations.emplace_back(thickness / 2);

            // 计算剩余厚度
            ret.left_over = thickness - safe_output_width;
            if (ret.left_over < 0) {
                ret.left_over = 0;  // 防止负值
            }
        }
        else
        {
            ret.left_over = thickness;
        }
        return ret;
    }
    else
    {
        return parent_->compute(thickness, bead_count);
    }
}

coord_t WideningBeadingStrategy::getOptimalThickness(coord_t bead_count) const
{
    return parent_->getOptimalThickness(bead_count);
}

coord_t WideningBeadingStrategy::getTransitionThickness(coord_t lower_bead_count) const
{
    if (lower_bead_count == 0)
    {
        return min_input_width_;
    }
    else
    {
        return parent_->getTransitionThickness(lower_bead_count);
    }
}

coord_t WideningBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    if (thickness < min_input_width_)
        return 0;
    coord_t ret = parent_->getOptimalBeadCount(thickness);
    if (thickness >= min_input_width_ && ret < 1)
        return 1;
    return ret;
}

coord_t WideningBeadingStrategy::getTransitioningLength(coord_t lower_bead_count) const
{
    return parent_->getTransitioningLength(lower_bead_count);
}

double WideningBeadingStrategy::getTransitionAnchorPos(coord_t lower_bead_count) const
{
    return parent_->getTransitionAnchorPos(lower_bead_count);
}

std::vector<coord_t> WideningBeadingStrategy::getNonlinearThicknesses(coord_t lower_bead_count) const
{
    std::vector<coord_t> ret;
    ret.emplace_back(min_output_width_);
    std::vector<coord_t> pret = parent_->getNonlinearThicknesses(lower_bead_count);
    ret.insert(ret.end(), pret.begin(), pret.end());
    return ret;
}

} // namespace cura
