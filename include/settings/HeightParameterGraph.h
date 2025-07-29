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

} // namespace cura

#endif // HEIGHT_PARAMETER_GRAPH_H
