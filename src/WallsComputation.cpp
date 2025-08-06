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
#include "raft.h"
#include "Slice.h"
#include "WallToolPaths.h"
#include "settings/HeightParameterGraph.h"
#include "settings/types/Ratio.h"
#include "settings/ZSeamConfig.h"
#include "sliceDataStorage.h"
#include "utils/Simplify.h" // We're simplifying the spiralized insets.
#include "utils/linearAlg2D.h"
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
void WallsComputation::generateWalls(SliceLayerPart* part, SectionType section_type, coord_t layer_z)
{
    size_t wall_count = settings_.get<size_t>("wall_line_count");
    if (wall_count == 0) // Early out if no walls are to be generated
    {
        part->print_outline = part->outline;
        part->inner_area = part->outline;
        return;
    }

    // === 检查是否应该使用螺旋模式 ===
    bool spiralize = settings_.get<bool>("magic_spiralize");
    if (spiralize)
    {
        // 检查是否定义了magic_spiralize_range
        std::string range_string = settings_.get<std::string>("magic_spiralize_range");

        // 如果magic_spiralize_range参数存在且不为空，尝试解析
        if (!range_string.empty())
        {
            HeightRangeList magic_spiralize_range = settings_.get<HeightRangeList>("magic_spiralize_range");

            // 如果解析成功（范围列表不为空），使用范围控制
            if (!magic_spiralize_range.isEmpty())
            {
                // 使用传入的正确Z坐标（支持可变层厚）
                // layer_z参数来自generateWalls(SliceLayer* layer)中的layer->printZ
                coord_t current_layer_z = layer_z;

                // 检查当前高度是否在指定范围内
                bool in_range = magic_spiralize_range.isInRange(current_layer_z);
                spiralize = in_range;

                // 调试信息
                spdlog::info("【螺旋范围控制】第{}层，高度{:.3f}mm（使用正确的printZ），范围检查结果：{}，螺旋模式：{}",
                             layer_nr_, current_layer_z / 1000.0, in_range ? "在范围内" : "超出范围", spiralize ? "启用" : "禁用");
            }
            else
            {
                // magic_spiralize_range参数存在但解析失败，回退到全螺旋模式
                // 这种情况下，用户开启了magic_spiralize但magic_spiralize_range解析失败
                // 按照用户要求：所有范围都使用螺旋模式
                const size_t initial_bottom_layers = settings_.get<size_t>("initial_bottom_layers");
                spiralize = layer_nr_ >= static_cast<LayerIndex>(initial_bottom_layers);

                // 调试信息
                spdlog::info("【螺旋范围控制】magic_spiralize_range解析失败，回退到全螺旋模式，第{}层，螺旋模式：{}",
                             layer_nr_, spiralize ? "启用" : "禁用");
            }
        }
        else
        {
            // 没有定义magic_spiralize_range参数，使用原有的initial_bottom_layers逻辑
            const size_t initial_bottom_layers = settings_.get<size_t>("initial_bottom_layers");
            spiralize = layer_nr_ >= static_cast<LayerIndex>(initial_bottom_layers);

            // 调试信息
            spdlog::info("【螺旋传统控制】第{}层，使用initial_bottom_layers逻辑，螺旋模式：{}",
                         layer_nr_, spiralize ? "启用" : "禁用");
        }
    }
    else
    {
        spdlog::info("【螺旋模式关闭】第{}层，magic_spiralize=false", layer_nr_);
    }

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
        spdlog::info("【墙体生成】第{}层，生成螺旋墙体", layer_nr_);
        const bool recompute_outline_based_on_outer_wall = settings_.get<bool>("support_enable") && ! settings_.get<bool>("fill_outline_gaps");

        generateSpiralInsets(part, line_width_0, wall_0_inset, recompute_outline_based_on_outer_wall, layer_z);
        if (layer_nr_ <= static_cast<LayerIndex>(settings_.get<size_t>("initial_bottom_layers")))
        {
            WallToolPaths wall_tool_paths(part->outline, line_width_0, line_width_x, wall_count, wall_0_inset, settings_, layer_nr_, section_type, layer_z);
            part->wall_toolpaths = wall_tool_paths.getToolPaths();
            part->inner_area = wall_tool_paths.getInnerContour();
        }
    }
    else
    {
        spdlog::info("【墙体生成】第{}层，生成正常墙体（包含inset/infill/skin）", layer_nr_);
        WallToolPaths wall_tool_paths(part->outline, line_width_0, line_width_x, wall_count, wall_0_inset, settings_, layer_nr_, section_type, layer_z);
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
    // 获取当前层的正确Z坐标（支持可变层厚）
    coord_t layer_z = layer->printZ;

    spdlog::info("【可变层厚修复】第{}层，使用正确的层Z坐标: {:.2f}mm（来自layer->printZ，支持可变层厚）", layer_nr_, INT2MM(layer_z));

    for (SliceLayerPart& part : layer->parts)
    {
        generateWalls(&part, section, layer_z);
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

void WallsComputation::generateSpiralInsets(SliceLayerPart* part, coord_t line_width_0, coord_t wall_0_inset, bool recompute_outline_based_on_outer_wall, coord_t layer_z)
{
    // 注意：此函数只在确定需要螺旋模式时才被调用，所以不需要再次检查螺旋条件

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
        spdlog::debug("=== only_spiralize_out_surface功能启用 ===");
        spdlog::debug("原始多边形数量: {}", spiral_outline.size());

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
        spdlog::debug("保留最外层多边形[{}]: 面积={:.2f}mm², 顶点数={}",
                    max_area_index, INT2MM2(max_area), outer_polygon.size());
        spdlog::debug("过滤后多边形数量: {}", spiral_outline.size());

        // 此时spiral_outline只包含一个多边形，即最外层的轮廓
        // 这将简化后续的螺旋路径生成，避免复杂的内部结构
    }

    // === 螺旋模式Z接缝点插值预处理 ===
    // 当启用插值功能时，在多边形初始化阶段插入插值点
    if (settings_.get<bool>("draw_z_seam_enable") &&
        settings_.get<bool>("z_seam_point_interpolation") &&
        !settings_.get<std::vector<Point3LL>>("draw_z_seam_points").empty())
    {
        spdlog::info("=== 螺旋模式Z接缝点插值预处理开始 ===");
        // 使用传入的正确Z坐标（支持可变层厚）
        coord_t current_layer_z = layer_z;

        Shape processed_spiral_outline;
        for (const Polygon& polygon : spiral_outline)
        {
            Polygon processed_polygon = insertZSeamInterpolationPointsForSpiral(polygon, current_layer_z);
            processed_spiral_outline.push_back(processed_polygon);
        }
        spiral_outline = processed_spiral_outline;
        spdlog::info("螺旋模式Z接缝点插值预处理完成，使用正确的printZ={:.2f}mm", INT2MM(current_layer_z));
    }

    // 使用处理后的轮廓生成螺旋wall
    part->spiral_wall = spiral_outline.offset(-line_width_0 / 2 - wall_0_inset);

    // 优化wall路径，防止打印机固件缓冲区不足，并减少CuraEngine的处理时间
    const ExtruderTrain& train_wall = settings_.get<ExtruderTrain&>("wall_0_extruder_nr");
    part->spiral_wall = Simplify(train_wall.settings_).polygon(part->spiral_wall);
    part->spiral_wall.removeDegenerateVerts();

    // === 螺旋模式Z接缝点插值后处理 ===
    // offset和simplify操作可能会丢失插值点，需要在最终的spiral_wall上重新插入
    if (settings_.get<bool>("draw_z_seam_enable") &&
        settings_.get<bool>("z_seam_point_interpolation") &&
        !settings_.get<std::vector<Point3LL>>("draw_z_seam_points").empty())
    {
        spdlog::info("=== 螺旋模式Z接缝点插值后处理开始 ===");
        // 使用传入的正确Z坐标（支持可变层厚）
        coord_t current_layer_z = layer_z;

        Shape processed_spiral_wall;
        for (const Polygon& polygon : part->spiral_wall)
        {
            Polygon processed_polygon = insertZSeamInterpolationPointsForSpiral(polygon, current_layer_z);
            processed_spiral_wall.push_back(processed_polygon);
        }
        part->spiral_wall = processed_spiral_wall;
        spdlog::info("螺旋模式Z接缝点插值后处理完成，使用正确的printZ={:.2f}mm", INT2MM(current_layer_z));
    }

    if (recompute_outline_based_on_outer_wall)
    {
        part->print_outline = part->spiral_wall.offset(line_width_0 / 2, ClipperLib::jtSquare);
    }
    else
    {
        part->print_outline = part->outline;
    }
}

Polygon WallsComputation::insertZSeamInterpolationPointsForSpiral(const Polygon& polygon, coord_t layer_z)
{
    // 获取Z接缝点列表
    auto z_seam_points = settings_.get<std::vector<Point3LL>>("draw_z_seam_points");

    // === 计算模型Z坐标（去除raft影响）===
    // 手绘Z接缝点是在原始模型上定义的，需要将切片Z坐标转换为模型Z坐标
    coord_t model_z = layer_z;
    const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;
    if (mesh_group_settings.get<EPlatformAdhesion>("adhesion_type") == EPlatformAdhesion::RAFT)
    {
        // 获取raft总厚度
        coord_t raft_total_thickness = Raft::getTotalThickness();

        // 获取raft间隙
        const ExtruderTrain& raft_surface_train = mesh_group_settings.get<ExtruderTrain&>("raft_surface_extruder_nr");
        coord_t raft_airgap = raft_surface_train.settings_.get<coord_t>("raft_airgap");

        // 计算模型实体高度
        model_z = layer_z - raft_total_thickness - raft_airgap;

        spdlog::debug("螺旋模式插值Raft计算: 层Z={:.2f}mm, Raft厚度={:.2f}mm, Raft间隙={:.2f}mm, 模型Z={:.2f}mm",
                     INT2MM(layer_z), INT2MM(raft_total_thickness), INT2MM(raft_airgap), INT2MM(model_z));
    }
    else
    {
        spdlog::debug("螺旋模式插值无Raft结构，模型Z={:.2f}mm（等于层Z）", INT2MM(model_z));
    }

    spdlog::debug("螺旋模式插值处理: 层Z={:.2f}mm, 模型Z={:.2f}mm, 多边形顶点数={}", INT2MM(layer_z), INT2MM(model_z), polygon.size());

    // 创建ZSeamConfig进行插值计算，使用模型Z坐标（去除raft影响）
    ZSeamConfig temp_config;
    temp_config.draw_z_seam_enable_ = true;
    temp_config.draw_z_seam_points_ = z_seam_points;
    temp_config.z_seam_point_interpolation_ = true;
    temp_config.draw_z_seam_grow_ = settings_.get<bool>("draw_z_seam_grow");
    temp_config.current_layer_z_ = model_z;

    // 尝试获取插值位置
    auto interpolated_pos = temp_config.getInterpolatedSeamPosition();
    if (!interpolated_pos.has_value())
    {
        spdlog::debug("螺旋模式插值计算失败，返回原多边形");
        return polygon;
    }

    Point2LL target_point = interpolated_pos.value();
    spdlog::info("螺旋模式插值目标点: ({:.2f}, {:.2f})", INT2MM(target_point.X), INT2MM(target_point.Y));

    // 在多边形中查找最近的线段并插入插值点
    const PointsSet& points = polygon;
    if (points.size() < 3)
    {
        spdlog::debug("螺旋模式多边形顶点数不足，返回原多边形");
        return polygon;
    }

    coord_t min_distance_sq = std::numeric_limits<coord_t>::max();
    size_t best_segment_idx = 0;
    Point2LL closest_point_on_segment;
    bool need_insert_point = false;

    // 遍历所有线段，找到最近的线段
    for (size_t i = 0; i < points.size(); ++i)
    {
        size_t next_i = (i + 1) % points.size();
        Point2LL segment_start = points[i];
        Point2LL segment_end = points[next_i];

        // 计算目标点到线段的最近点
        Point2LL closest_point = LinearAlg2D::getClosestOnLineSegment(target_point, segment_start, segment_end);
        coord_t distance_sq = vSize2(target_point - closest_point);

        if (distance_sq < min_distance_sq)
        {
            min_distance_sq = distance_sq;
            best_segment_idx = i;
            closest_point_on_segment = closest_point;

            // 检查最近点是否是线段端点
            coord_t dist_to_start = vSize2(closest_point - segment_start);
            coord_t dist_to_end = vSize2(closest_point - segment_end);
            const coord_t epsilon_sq = 100; // 0.01mm的平方

            need_insert_point = (dist_to_start > epsilon_sq && dist_to_end > epsilon_sq);
        }
    }

    spdlog::info("螺旋模式最近线段: 索引{}, 距离: {:.2f}mm", best_segment_idx, INT2MM(std::sqrt(min_distance_sq)));

    if (need_insert_point)
    {
        // 在线段中间插入新点
        Polygon modified_polygon = polygon;
        size_t insert_idx = best_segment_idx + 1;

        // 获取可修改的点集合
        ClipperLib::Path modified_points = modified_polygon.getPoints();
        modified_points.insert(modified_points.begin() + insert_idx, closest_point_on_segment);

        // 创建新的多边形
        Polygon result_polygon(std::move(modified_points), true);

        spdlog::debug("螺旋模式在索引{}插入新点: ({:.2f}, {:.2f})",
                    insert_idx, INT2MM(closest_point_on_segment.X), INT2MM(closest_point_on_segment.Y));
        spdlog::debug("螺旋模式多边形顶点数: {} -> {}", polygon.size(), result_polygon.size());

        return result_polygon;
    }
    else
    {
        spdlog::debug("螺旋模式最近点是现有顶点，无需插入新点");
        return polygon;
    }
}

} // namespace cura
