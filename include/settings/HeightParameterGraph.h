// Copyright (c) 2024 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#ifndef HEIGHT_PARAMETER_GRAPH_H
#define HEIGHT_PARAMETER_GRAPH_H

#include <cassert>
#include <vector>

#include "utils/Coord_t.h"

namespace cura
{

/*!
 * Class representing a graph matching a height to a parameter value.
 * The graph generally consists of several linear line segments between points at which the height and parameter are matched.
 * Used for user-defined thickness, temperature, and speed ratio control.
 */
class HeightParameterGraph
{
public:
    struct Datum
    {
        coord_t height_; //!< The height in microns (converted from mm)
        double parameter_; //!< The parameter value (thickness in mm, temperature in °C, or speed ratio in %)
        Datum(const coord_t height, const double parameter)
            : height_(height)
            , parameter_(parameter)
        {
        }
    };

    std::vector<Datum> data_; //!< The points of the graph between which the graph is linearly interpolated

    /*!
     * Get the parameter value corresponding to a specific height.
     *
     * For heights outside of the chart, the parameter at the minimal or maximal height is returned.
     * When the graph is empty, the default_parameter is returned.
     *
     * \param height the height in microns
     * \param default_parameter The default parameter value (backward compatibility for when the graph fails)
     * \return the corresponding parameter value
     */
    double getParameter(const coord_t height, const double default_parameter) const;

    /*!
     * Check if the graph is empty
     * \return true if no data points are defined
     */
    bool isEmpty() const;
};

/*!
 * Class representing a list of height ranges for magic_spiralize_range parameter.
 * Used to determine if a specific height should use spiralize mode.
 */
class HeightRangeList
{
public:
    struct Range
    {
        coord_t min_height_; //!< The minimum height in microns (converted from mm)
        coord_t max_height_; //!< The maximum height in microns (converted from mm)

        Range(const coord_t min_height, const coord_t max_height)
            : min_height_(min_height)
            , max_height_(max_height)
        {
        }

        /*!
         * Check if a height is within this range
         * \param height the height in microns
         * \return true if height is within [min_height_, max_height_]
         */
        bool contains(const coord_t height) const
        {
            return height >= min_height_ && height <= max_height_;
        }
    };

    std::vector<Range> ranges_; //!< The list of height ranges

    /*!
     * Check if a specific height is within any of the defined ranges.
     * \param height the height in microns
     * \return true if height is within any range
     */
    bool isInRange(const coord_t height) const;

    /*!
     * Check if the range list is empty
     * \return true if no ranges are defined
     */
    bool isEmpty() const;

    /*!
     * Add a new range to the list
     * \param min_height minimum height in microns
     * \param max_height maximum height in microns
     */
    void addRange(const coord_t min_height, const coord_t max_height);

    /*!
     * Sort ranges by minimum height (for consistency)
     */
    void sortRanges();
};

} // namespace cura

#endif // HEIGHT_PARAMETER_GRAPH_H
