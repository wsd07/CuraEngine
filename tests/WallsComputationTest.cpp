// Copyright (c) 2023 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#include "WallsComputation.h" //Unit under test.

#include <unordered_set>

#include <range/v3/view/join.hpp>
#include <scripta/logger.h>

#include <gtest/gtest.h>

#include "InsetOrderOptimizer.h" //Unit also under test.
#include "geometry/OpenPolyline.h"
#include "geometry/Polygon.h" //To create example polygons.
#include "settings/Settings.h" //Settings to generate walls with.
#include "sliceDataStorage.h" //Sl
#include "slicer.h"

#ifdef WALLS_COMPUTATION_TEST_SVG_OUTPUT
#include <cstdlib>

#include "utils/SVG.h"
#endif // WALLS_COMPUTATION_TEST_SVG_OUTPUT

// NOLINTBEGIN(*-magic-numbers)
namespace cura
{
/*!
 * Fixture that provides a basis for testing wall computation.
 */
class WallsComputationTest : public testing::Test
{
public:
    /*!
     * Settings to slice with. This is linked in the walls_computation fixture.
     */
    Settings settings;

    /*!
     * WallsComputation instance to test with. The layer index will be 100.
     */
    WallsComputation walls_computation;

    /*!
     * Basic 10x10mm square shape to work with.
     */
    Shape square_shape;

    /*!
     * A rectangle enclosing two triangular holes;
     */
    Shape ff_holes;

    WallsComputationTest()
        : walls_computation(settings, LayerIndex(100))
    {
        square_shape.emplace_back();
        square_shape.back().emplace_back(0, 0);
        square_shape.back().emplace_back(MM2INT(20), 0);
        square_shape.back().emplace_back(MM2INT(20), MM2INT(20));
        square_shape.back().emplace_back(0, MM2INT(20));

        ff_holes.emplace_back();
        ff_holes.back().emplace_back(0, 0);
        ff_holes.back().emplace_back(5000, 0);
        ff_holes.back().emplace_back(5000, 5000);
        ff_holes.back().emplace_back(0, 5000);
        ff_holes.emplace_back();
        ff_holes.back().emplace_back(6000, 9000);
        ff_holes.back().emplace_back(9000, 7500);
        ff_holes.back().emplace_back(6000, 6000);

        // Settings for a simple 2 walls, about as basic as possible.
        settings.add("alternate_extra_perimeter", "false");
        settings.add("fill_outline_gaps", "false");
        settings.add("initial_layer_line_width_factor", "100");
        settings.add("magic_spiralize", "false");
        settings.add("meshfix_maximum_deviation", "0.1");
        settings.add("meshfix_maximum_extrusion_area_deviation", "0.01");
        settings.add("meshfix_fluid_motion_enabled", "false");
        settings.add("meshfix_maximum_resolution", "0.01");
        settings.add("min_wall_line_width", "0.3");
        settings.add("min_bead_width", "0");
        settings.add("min_feature_size", "0");
        settings.add("wall_0_extruder_nr", "0");
        settings.add("wall_0_inset", "0");
        settings.add("wall_line_count", "2");
        settings.add("wall_line_width_0", "0.4");
        settings.add("wall_line_width_x", "0.4");
        settings.add("min_even_wall_line_width", "0.34");
        settings.add("min_odd_wall_line_width", "0.34");
        settings.add("wall_transition_angle", "10");
        settings.add("wall_transition_filter_distance", "1");
        settings.add("wall_transition_filter_deviation", ".2");
        settings.add("wall_transition_length", "1");
        settings.add("wall_x_extruder_nr", "0");
        settings.add("wall_distribution_count", "2");

        // Add parameters for small contour filtering
        settings.add("minimum_polygon_circumference", "0");
        settings.add("minimum_polygon_area", "0");
    }
};

/*!
 * Tests if something is generated in the basic happy case.
 */
TEST_F(WallsComputationTest, GenerateWallsForLayerSinglePart)
{
    auto layers = std::vector<SlicerLayer>(200, SlicerLayer{});
    scripta::setAll(layers);
    SliceLayer layer;
    layer.parts.emplace_back();
    SliceLayerPart& part = layer.parts.back();
    part.outline.push_back(square_shape);

    // Run the test.
    walls_computation.generateWalls(&layer, SectionType::WALL);

    // Verify that something was generated.
    EXPECT_FALSE(part.wall_toolpaths.empty()) << "There must be some walls.";
    EXPECT_GT(part.print_outline.area(), 0) << "The print outline must encompass the outer wall, so it must be more than 0.";
    EXPECT_LE(part.print_outline.area(), square_shape.area()) << "The print outline must stay within the bounds of the original part.";
    EXPECT_GT(part.inner_area.area(), 0)
        << "The inner area must be within the innermost wall. There are not enough walls to fill the entire part, so there is a positive inner area.";
    EXPECT_EQ(layer.parts.size(), 1) << "There is still just 1 part.";
}

/*!
 * Tests if the inner area is properly set.
 */
TEST_F(WallsComputationTest, GenerateWallsZeroWalls)
{
    settings.add("wall_line_count", "0");
    SliceLayer layer;
    layer.parts.emplace_back();
    SliceLayerPart& part = layer.parts.back();
    part.outline.push_back(square_shape);

    // Run the test.
    walls_computation.generateWalls(&layer, SectionType::WALL);

    // Verify that there is still an inner area, outline and parts.
    EXPECT_EQ(part.inner_area.area(), square_shape.area()) << "There are no walls, so the inner area (for infill/skin) needs to be the entire part.";
    EXPECT_EQ(part.print_outline.area(), square_shape.area()) << "There are no walls, so the print outline encompasses the inner area exactly.";
    EXPECT_EQ(part.outline.area(), square_shape.area()) << "The outline is not modified.";
    EXPECT_EQ(layer.parts.size(), 1) << "There is still just 1 part.";
}

/*!
 * Tests if the inner area is properly set.
 */
TEST_F(WallsComputationTest, WallToolPathsGetWeakOrder)
{
    settings.add("wall_line_count", "5");
    SliceLayer layer;
    layer.parts.emplace_back();
    SliceLayerPart& part = layer.parts.back();
    part.outline.push_back(ff_holes);

    // Run the test.
    walls_computation.generateWalls(&layer, SectionType::WALL);

    const bool outer_to_inner = false;
    std::vector<ExtrusionLine> all_paths;
    for (auto& line : part.wall_toolpaths | ranges::views::join)
    {
        all_paths.emplace_back(line);
    }
    auto order = InsetOrderOptimizer::getRegionOrder(all_paths, outer_to_inner);

    // Verify that something was generated.
    EXPECT_FALSE(part.wall_toolpaths.empty()) << "There must be some walls.";
    EXPECT_GT(part.print_outline.area(), 0) << "The print outline must encompass the outer wall, so it must be more than 0.";
    EXPECT_LE(part.print_outline.area(), ff_holes.area()) << "The print outline must stay within the bounds of the original part.";
    EXPECT_GE(part.inner_area.area(), 0) << "The inner area can never have negative area.";
    EXPECT_EQ(layer.parts.size(), 1) << "There is still just 1 part.";

#ifdef WALLS_COMPUTATION_TEST_SVG_OUTPUT
    {
        SVG svg("/tmp/wall_order.svg", AABB(part.outline));
        for (const VariableWidthLines& inset : part.wall_toolpaths)
        {
            for (const ExtrusionLine& line : inset)
            {
                if (line.is_odd)
                {
                    svg.writePolyline(line.toPolygon(), SVG::Color::YELLOW);
                    svg.writePoints(line.toPolygon(), true);
                }
                else
                    svg.writePolygon(line.toPolygon(), SVG::Color::GREEN);
            }
        }
        svg.writePolygons(part.outline, SVG::Color::RED);
        svg.writePolygons(part.inner_area, SVG::Color::YELLOW);
        svg.nextLayer();
        for (auto [first, second] : order)
        {
            if (! second->is_odd)
                svg.writeArrow(first->front().p, (++second->begin())->p, SVG::Color::BLUE);
        }
        svg.nextLayer();
        for (auto [first, second] : order)
        {
            if (second->is_odd)
                svg.writeArrow(first->front().p, (++second->begin())->p, SVG::Color::MAGENTA);
        }
    }
#endif // WALLS_COMPUTATION_TEST_SVG_OUTPUT

    size_t n_paths = 0;
    for (auto& lines : part.wall_toolpaths)
    {
        for (auto& line : lines)
        {
            if (! line.empty())
            {
                n_paths++;
            }
        }
    }
    EXPECT_GT(order.size(), 0) << "There should be ordered pairs!";
    std::unordered_set<const ExtrusionLine*> has_order_info(part.wall_toolpaths.size());
    for (auto [from, to] : order)
    {
        has_order_info.emplace(from);
        has_order_info.emplace(to);
    }
    EXPECT_EQ(has_order_info.size(), n_paths) << "Every path should have order information.";
}

/*!
 * Tests if small wall toolpaths are properly filtered after beading.
 */
TEST_F(WallsComputationTest, FilterSmallWallToolpaths)
{
    // Create a shape with a very small feature that should be filtered out
    Shape shape_with_small_feature;

    // Main large square (20x20mm)
    shape_with_small_feature.emplace_back();
    shape_with_small_feature.back().emplace_back(0, 0);
    shape_with_small_feature.back().emplace_back(MM2INT(20), 0);
    shape_with_small_feature.back().emplace_back(MM2INT(20), MM2INT(20));
    shape_with_small_feature.back().emplace_back(0, MM2INT(20));

    // Small feature (1x1mm) that should be filtered out
    shape_with_small_feature.emplace_back();
    shape_with_small_feature.back().emplace_back(MM2INT(25), MM2INT(5));
    shape_with_small_feature.back().emplace_back(MM2INT(26), MM2INT(5));
    shape_with_small_feature.back().emplace_back(MM2INT(26), MM2INT(6));
    shape_with_small_feature.back().emplace_back(MM2INT(25), MM2INT(6));

    SliceLayer layer;
    layer.parts.emplace_back();
    SliceLayerPart& part = layer.parts.back();
    part.outline.push_back(shape_with_small_feature);

    // Test without filtering (baseline)
    settings.add("minimum_polygon_circumference", "0");
    settings.add("minimum_polygon_area", "0");
    walls_computation.generateWalls(&layer, SectionType::WALL);

    size_t original_wall_count = 0;
    for (const auto& wall_lines : part.wall_toolpaths)
    {
        original_wall_count += wall_lines.size();
    }

    // Reset for filtered test
    part.wall_toolpaths.clear();
    part.inner_area.clear();
    part.print_outline.clear();

    // Test with filtering enabled (filter paths shorter than 5mm)
    settings.add("minimum_polygon_circumference", "5000"); // 5mm in micrometers
    settings.add("minimum_polygon_area", "0");
    walls_computation.generateWalls(&layer, SectionType::WALL);

    size_t filtered_wall_count = 0;
    for (const auto& wall_lines : part.wall_toolpaths)
    {
        filtered_wall_count += wall_lines.size();
    }

    // Verify that some walls were filtered out
    EXPECT_LT(filtered_wall_count, original_wall_count) << "Small wall toolpaths should be filtered out.";
    EXPECT_GT(filtered_wall_count, 0) << "Main walls should still remain.";

    // Verify that all remaining walls meet the minimum length requirement
    for (const auto& wall_lines : part.wall_toolpaths)
    {
        for (const auto& line : wall_lines)
        {
            coord_t line_length = line.length();
            EXPECT_GE(line_length, 5000) << "All remaining walls should meet minimum length requirement.";
        }
    }
}

} // namespace cura
// NOLINTEND(*-magic-numbers)
