// Copyright (c) 2023 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#include "WallsComputation.h"

#include <fstream>
#include <iostream>

#include <fmt/format.h>
#include <range/v3/to_container.hpp>
#include <range/v3/view/c_str.hpp>
#include <range/v3/view/join.hpp>
#include <range/v3/view/transform.hpp>

#include "Application.h"
#include "ExtruderTrain.h"
#include "Slice.h"
#include "WallToolPaths.h"
#include "settings/types/Ratio.h"
#include "sliceDataStorage.h"
#include "utils/Simplify.h" // We're simplifying the spiralized insets.
#include <spdlog/spdlog.h>
#include "utils/math.h" // For INT2MM2

namespace cura
{

WallsComputation::WallsComputation(const Settings& settings, const LayerIndex layer_nr)
    : settings_(settings)
    , layer_nr_(layer_nr)
{
}

/*
 * This function is executed in a parallel region based on layer_nr.
 * When modifying make sure any changes does not introduce data races.
 *
 * generateWalls only reads and writes data for the current layer
 */
void WallsComputation::generateWalls(SliceLayerPart* part, SectionType section_type)
{
    size_t wall_count = settings_.get<size_t>("wall_line_count");
    if (wall_count == 0) // Early out if no walls are to be generated
    {
        part->print_outline = part->outline;
        part->inner_area = part->outline;
        return;
    }

    const bool spiralize = settings_.get<bool>("magic_spiralize");
    const size_t alternate = ((layer_nr_ % 2) + 2) % 2;
    if (spiralize && layer_nr_ < LayerIndex(settings_.get<size_t>("initial_bottom_layers"))
        && alternate == 1) // Add extra insets every 2 layers when spiralizing. This makes bottoms of cups watertight.
    {
        wall_count += 5;
    }
    if (settings_.get<bool>("alternate_extra_perimeter"))
    {
        wall_count += alternate;
    }

    const bool first_layer = layer_nr_ == 0;
    const Ratio line_width_0_factor = first_layer ? settings_.get<ExtruderTrain&>("wall_0_extruder_nr").settings_.get<Ratio>("initial_layer_line_width_factor") : 1.0_r;
    const coord_t line_width_0 = settings_.get<coord_t>("wall_line_width_0") * line_width_0_factor;
    const coord_t wall_0_inset = settings_.get<coord_t>("wall_0_inset");

    const Ratio line_width_x_factor = first_layer ? settings_.get<ExtruderTrain&>("wall_x_extruder_nr").settings_.get<Ratio>("initial_layer_line_width_factor") : 1.0_r;
    const coord_t line_width_x = settings_.get<coord_t>("wall_line_width_x") * line_width_x_factor;

    // When spiralizing, generate the spiral insets using simple offsets instead of generating toolpaths
    if (spiralize)
    {
        const bool recompute_outline_based_on_outer_wall = settings_.get<bool>("support_enable") && ! settings_.get<bool>("fill_outline_gaps");

        generateSpiralInsets(part, line_width_0, wall_0_inset, recompute_outline_based_on_outer_wall);
        if (layer_nr_ <= static_cast<LayerIndex>(settings_.get<size_t>("initial_bottom_layers")))
        {
            WallToolPaths wall_tool_paths(part->outline, line_width_0, line_width_x, wall_count, wall_0_inset, settings_, layer_nr_, section_type);
            part->wall_toolpaths = wall_tool_paths.getToolPaths();
            part->inner_area = wall_tool_paths.getInnerContour();
        }
    }
    else
    {
        WallToolPaths wall_tool_paths(part->outline, line_width_0, line_width_x, wall_count, wall_0_inset, settings_, layer_nr_, section_type);
        part->wall_toolpaths = wall_tool_paths.getToolPaths();
        part->inner_area = wall_tool_paths.getInnerContour();
    }

    part->outline = SingleShape{ Simplify(settings_).polygon(part->outline) };
    part->print_outline = part->outline;
}

/*
 * This function is executed in a parallel region based on layer_nr.
 * When modifying make sure any changes does not introduce data races.
 *
 * generateWalls only reads and writes data for the current layer
 */
void WallsComputation::generateWalls(SliceLayer* layer, SectionType section)
{
    for (SliceLayerPart& part : layer->parts)
    {
        generateWalls(&part, section);
    }

    // Remove the parts which did not generate a wall. As these parts are too small to print,
    //  and later code can now assume that there is always minimal 1 wall line.
    bool check_wall_and_spiral = settings_.get<size_t>("wall_line_count") >= 1 && ! settings_.get<bool>("fill_outline_gaps");
    auto iterator_remove = std::remove_if(
        layer->parts.begin(),
        layer->parts.end(),
        [&check_wall_and_spiral](const SliceLayerPart& part)
        {
            return (check_wall_and_spiral && part.wall_toolpaths.empty() && part.spiral_wall.empty()) || part.outline.empty() || part.print_outline.empty();
        });
    layer->parts.erase(iterator_remove, layer->parts.end());
}

void WallsComputation::generateSpiralInsets(SliceLayerPart* part, coord_t line_width_0, coord_t wall_0_inset, bool recompute_outline_based_on_outer_wall)
{
    // === 检查是否启用only_spiralize_out_surface功能 ===
    // 当启用时，只保留最外层的多边形，舍弃内部的多边形
    // 如果参数未设置，默认为false（保持原有行为）
    bool only_spiralize_out_surface = false;
    try {
        only_spiralize_out_surface = settings_.get<bool>("only_spiralize_out_surface");
    } catch (...) {
        // 参数未设置，使用默认值false
        only_spiralize_out_surface = false;
    }

    Shape spiral_outline = part->outline;

    // 添加调试信息，显示当前处理的多边形数量
    spdlog::debug("generateSpiralInsets: only_spiralize_out_surface={}, 多边形数量={}",
                 only_spiralize_out_surface, spiral_outline.size());

    // === 核心功能：only_spiralize_out_surface多边形筛选 ===
    // 当启用此功能且当前层有多个多边形时，只保留最外层的多边形
    // 这对于有内部孔洞或复杂截面的模型特别有用，可以简化螺旋路径
    if (only_spiralize_out_surface && spiral_outline.size() > 1)
    {
        // 输出功能启用信息和原始状态
        spdlog::info("=== only_spiralize_out_surface功能启用 ===");
        spdlog::info("原始多边形数量: {}", spiral_outline.size());

        // === 算法：基于面积大小识别最外层多边形 ===
        // 原理：在3D切片中，最外层的轮廓通常具有最大的面积
        // 注意：在Cura的坐标系统中：
        //   - 外轮廓（实体边界）：逆时针方向，面积为正值
        //   - 内轮廓（孔洞）：顺时针方向，面积为负值
        // 我们使用绝对值来比较，确保能正确识别最大的轮廓
        coord_t max_area = 0;        // 记录找到的最大面积
        size_t max_area_index = 0;   // 记录最大面积多边形的索引

        // 遍历所有多边形，计算面积并找到最大的
        for (size_t i = 0; i < spiral_outline.size(); ++i)
        {
            // 计算当前多边形的面积（使用绝对值处理正负面积）
            coord_t area = std::abs(spiral_outline[i].area());

            // 输出每个多边形的详细信息用于调试
            spdlog::debug("多边形[{}]: 面积={:.2f}mm², 顶点数={}",
                         i, INT2MM2(area), spiral_outline[i].size());

            // 更新最大面积记录
            if (area > max_area)
            {
                max_area = area;
                max_area_index = i;
            }
        }

        // === 执行筛选：只保留面积最大的多边形 ===
        // 这个多边形通常就是模型的最外层轮廓
        Polygon outer_polygon = spiral_outline[max_area_index];
        spiral_outline.clear();                    // 清空原有的所有多边形
        spiral_outline.push_back(outer_polygon);   // 只添加最外层多边形

        // 输出筛选结果的详细信息
        spdlog::info("保留最外层多边形[{}]: 面积={:.2f}mm², 顶点数={}",
                    max_area_index, INT2MM2(max_area), outer_polygon.size());
        spdlog::info("过滤后多边形数量: {}", spiral_outline.size());

        // 此时spiral_outline只包含一个多边形，即最外层的轮廓
        // 这将简化后续的螺旋路径生成，避免复杂的内部结构
    }

    // 使用处理后的轮廓生成螺旋wall
    part->spiral_wall = spiral_outline.offset(-line_width_0 / 2 - wall_0_inset);

    // 优化wall路径，防止打印机固件缓冲区不足，并减少CuraEngine的处理时间
    const ExtruderTrain& train_wall = settings_.get<ExtruderTrain&>("wall_0_extruder_nr");
    part->spiral_wall = Simplify(train_wall.settings_).polygon(part->spiral_wall);
    part->spiral_wall.removeDegenerateVerts();

    if (recompute_outline_based_on_outer_wall)
    {
        part->print_outline = part->spiral_wall.offset(line_width_0 / 2, ClipperLib::jtSquare);
    }
    else
    {
        part->print_outline = part->outline;
    }
}

} // namespace cura
