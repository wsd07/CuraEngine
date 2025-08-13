// Copyright (c) 2025 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef FIXED_OUTER_WALL_BEADING_STRATEGY_H
#define FIXED_OUTER_WALL_BEADING_STRATEGY_H

#include "../settings/types/Ratio.h"
#include "BeadingStrategy.h"

namespace cura
{
/*!
 * A meta-beading-strategy that ensures outer walls have completely fixed width.
 * 
 * This strategy is designed for INNER_WALL_SKIN mode where:
 * - Outer walls (first and last) use completely fixed width, never change
 * - Inner walls use the parent beading strategy for optimization
 * - This provides the best surface quality while optimizing internal structure
 */
class FixedOuterWallBeadingStrategy : public BeadingStrategy
{
public:
    /*!
     * \param fixed_outer_width        Fixed width for outer walls, never changes
     * \param minimum_variable_line_ratio Minimum factor that the variable line might deviate from the optimal width.
     * \param parent                   Parent strategy for inner walls
     */
    FixedOuterWallBeadingStrategy(const coord_t fixed_outer_width, const Ratio minimum_variable_line_ratio, BeadingStrategyPtr parent);

    virtual ~FixedOuterWallBeadingStrategy() override = default;

    Beading compute(coord_t thickness, coord_t bead_count) const override;

    coord_t getOptimalThickness(coord_t bead_count) const override;
    coord_t getTransitionThickness(coord_t lower_bead_count) const override;
    coord_t getOptimalBeadCount(coord_t thickness) const override;
    coord_t getTransitioningLength(coord_t lower_bead_count) const override;
    double getTransitionAnchorPos(coord_t lower_bead_count) const override;

    std::string toString() const override;

protected:
    BeadingStrategyPtr parent_;
    coord_t fixed_outer_width_;
    Ratio minimum_variable_line_ratio_;
};

} // namespace cura
#endif // FIXED_OUTER_WALL_BEADING_STRATEGY_H
