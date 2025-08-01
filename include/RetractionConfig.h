// Copyright (c) 2022 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef RETRACTION_CONFIG_H
#define RETRACTION_CONFIG_H

#include "settings/types/Ratio.h"
#include "settings/types/Velocity.h"
#include "utils/Coord_t.h"

namespace cura
{

/*!
 * The retraction configuration used in the GCodePathConfig of each feature (and the travel config)
 */
class RetractionConfig
{
public:
    double distance; //!< The distance retracted (in mm)
    Ratio retract_during_travel; //!< The ratio of retraction to be performed while traveling
    bool keep_retracting_during_travel; //! Whether we should spread the retraction over the whole travel move
    Ratio prime_during_travel; //!< The ratio of priming to be performed while traveling
    Velocity speed; //!< The speed with which to retract (in mm/s)
    Velocity primeSpeed; //!< the speed with which to unretract (in mm/s)
    double prime_volume; //!< the amount of material primed after unretracting (in mm^3)
    coord_t zHop; //!< the amount with which to lift the head during a retraction-travel
    coord_t retraction_min_travel_distance; //!< Minimal distance traversed to even consider retracting (in micron)
    double retraction_extrusion_window; //!< Window of mm extruded filament in which to limit the amount of retractions
    size_t retraction_count_max; //!< The maximum amount of retractions allowed to occur in the RetractionConfig::retraction_extrusion_window
};


} // namespace cura

#endif // RETRACTION_CONFIG_H
