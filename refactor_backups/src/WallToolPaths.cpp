// Copyright (c) 2023 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#include "WallToolPaths.h"

#include <algorithm> //For std::partition_copy and std::min_element.
#include <unordered_set>

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/transform.hpp>
#include <scripta/logger.h>

#include "ExtruderTrain.h"
#include "SkeletalTrapezoidation.h"
#include "utils/ExtrusionLineStitcher.h"
#include "utils/Simplify.h"
#include "utils/SparsePointGrid.h"
#include "utils/DebugManager.h" //To stitch the inner contour.
#include "utils/actions/smooth.h"
#include "utils/linearAlg2D.h"
#include "utils/polygonUtils.h"
#include "settings/ZSeamConfig.h"

namespace cura
{

WallToolPaths::WallToolPaths(
    const Shape& outline,
    const coord_t nominal_bead_width,
    const size_t inset_count,
    const coord_t wall_0_inset,
    const Settings& settings,
    const int layer_idx,
    SectionType section_type,
    const coord_t layer_z)
    : outline_(outline)
    , bead_width_0_(nominal_bead_width)
    , bead_width_x_(nominal_bead_width)
    , inset_count_(inset_count)
    , wall_0_inset_(wall_0_inset)
    , print_thin_walls_(settings.get<bool>("fill_outline_gaps"))
    , min_feature_size_(settings.get<coord_t>("min_feature_size"))
    , min_bead_width_(settings.get<coord_t>("min_bead_width"))
    , small_area_length_(INT2MM(static_cast<double>(nominal_bead_width) / 2))
    , toolpaths_generated_(false)
    , settings_(settings)
    , layer_idx_(layer_idx)
    , layer_z_(layer_z)
    , section_type_(section_type)
{
}

WallToolPaths::WallToolPaths(
    const Shape& outline,
    const coord_t bead_width_0,
    const coord_t bead_width_x,
    const size_t inset_count,
    const coord_t wall_0_inset,
    const Settings& settings,
    const int layer_idx,
    SectionType section_type,
    const coord_t layer_z)
    : outline_(outline)
    , bead_width_0_(bead_width_0)
    , bead_width_x_(bead_width_x)
    , inset_count_(inset_count)
    , wall_0_inset_(wall_0_inset)
    , print_thin_walls_(settings.get<bool>("fill_outline_gaps"))
    , min_feature_size_(settings.get<coord_t>("min_feature_size"))
    , min_bead_width_(settings.get<coord_t>("min_bead_width"))
    , small_area_length_(INT2MM(static_cast<double>(bead_width_0) / 2))
    , toolpaths_generated_(false)
    , settings_(settings)
    , layer_idx_(layer_idx)
    , layer_z_(layer_z)
    , section_type_(section_type)
{
}

const std::vector<VariableWidthLines>& WallToolPaths::generate()
{
    const coord_t allowed_distance = settings_.get<coord_t>("meshfix_maximum_deviation");

    // Sometimes small slivers of polygons mess up the prepared_outline. By performing an open-close operation
    // with half the minimum printable feature size or minimum line width, these slivers are removed, while still
    // keeping enough information to not degrade the print quality;
    // These features can't be printed anyhow. See PR CuraEngine#1811 for some screenshots
    const coord_t open_close_distance
        = settings_.get<bool>("fill_outline_gaps") ? settings_.get<coord_t>("min_feature_size") / 2 - 5 : settings_.get<coord_t>("min_wall_line_width") / 2 - 5;
    const coord_t epsilon_offset = (allowed_distance / 2) - 1;
    const auto transitioning_angle = settings_.get<AngleRadians>("wall_transition_angle");
    constexpr coord_t discretization_step_size = MM2INT(0.8);

    // Simplify outline for boost::voronoi consumption. Absolutely no self intersections or near-self intersections allowed:
    // TODO: Open question: Does this indeed fix all (or all-but-one-in-a-million) cases for manifold but otherwise possibly complex polygons?
    Shape prepared_outline = outline_.offset(-open_close_distance).offset(open_close_distance * 2).offset(-open_close_distance);
    scripta::log("prepared_outline_0", prepared_outline, section_type_, layer_idx_);
    prepared_outline.removeSmallAreas(small_area_length_ * small_area_length_, false);
    prepared_outline = Simplify(settings_).polygon(prepared_outline);
    if (settings_.get<bool>("meshfix_fluid_motion_enabled") && section_type_ != SectionType::SUPPORT)
    {
        // No need to smooth support walls
        auto smoother = actions::smooth(settings_);
        for (Polygon& polygon : prepared_outline)
        {
            polygon.setPoints(smoother(polygon.getPoints()));
        }
    }

    PolygonUtils::fixSelfIntersections(epsilon_offset, prepared_outline);
    prepared_outline.removeDegenerateVerts();
    prepared_outline.removeColinearEdges(AngleRadians(0.005));
    // Removing collinear edges may introduce self intersections, so we need to fix them again
    PolygonUtils::fixSelfIntersections(epsilon_offset, prepared_outline);
    prepared_outline.removeDegenerateVerts();
    prepared_outline = prepared_outline.unionPolygons();
    prepared_outline = Simplify(settings_).polygon(prepared_outline);

    if (prepared_outline.area() <= 0)
    {
        assert(toolpaths_.empty());
        return toolpaths_;
    }

    prepared_outline = prepared_outline.removeNearSelfIntersections();

    // === 新策略：在多边形预处理阶段插入Z接缝插值点 ===
    // 这确保插值点在所有后续处理中都存在
    if (settings_.get<bool>("draw_z_seam_enable") && settings_.get<bool>("z_seam_point_interpolation"))
    {
        spdlog::debug("=== 开始Z接缝点预处理插值 ===");

        // 获取正确的层Z坐标：优先使用传入的layer_z_，否则回退到简单计算
        coord_t layer_z;
        if (layer_z_ >= 0)
        {
            // 使用传入的正确Z坐标（支持可变层厚）
            layer_z = layer_z_;
            spdlog::debug("使用传入的层Z坐标: {:.2f}mm（支持可变层厚）", INT2MM(layer_z));
        }
        else
        {
            // 回退到简单计算（向后兼容）
            layer_z = layer_idx_ * settings_.get<coord_t>("layer_height");
            spdlog::debug("使用简单计算的层Z坐标: {:.2f}mm（固定层厚模式）", INT2MM(layer_z));
        }

        Shape processed_outline;
        for (const Polygon& polygon : prepared_outline)
        {
            Polygon processed_polygon = insertZSeamInterpolationPoints(polygon, settings_, layer_z);
            processed_outline.push_back(processed_polygon);
        }
        prepared_outline = processed_outline;
        spdlog::debug("Z接缝点预处理完成，处理了{}个多边形", prepared_outline.size());
    }

    const coord_t wall_transition_length = settings_.get<coord_t>("wall_transition_length");

    // === 参数验证和修复 ===
    // 检查min_bead_width是否过小，防止BeadingStrategy计算错误
    const coord_t original_min_bead_width = settings_.get<coord_t>("min_bead_width");
    const coord_t absolute_minimum = MM2INT(0.1);  // 0.1mm绝对最小值
    const coord_t stability_minimum = std::max(bead_width_x_, bead_width_0_) * 0.4;  // 40%稳定性约束
    const coord_t safe_min_bead_width = std::max({original_min_bead_width, absolute_minimum, stability_minimum});

    if (original_min_bead_width != safe_min_bead_width) {
        CURA_WARN("【参数修复】min_bead_width从{:.2f}mm调整为{:.2f}mm（安全下限：40%喷头直径），防止BeadingStrategy计算错误",
                     INT2MM(original_min_bead_width), INT2MM(safe_min_bead_width));
    }

    // When to split the middle wall into two:
    const double min_even_wall_line_width = settings_.get<double>("min_even_wall_line_width");
    const double wall_line_width_0 = settings_.get<double>("wall_line_width_0");
    const Ratio wall_split_middle_threshold = std::max(1.0, std::min(99.0, 100.0 * (2.0 * min_even_wall_line_width - wall_line_width_0) / wall_line_width_0)) / 100.0;

    // When to add a new middle in between the innermost two walls:
    const double min_odd_wall_line_width = settings_.get<double>("min_odd_wall_line_width");
    const double wall_line_width_x = settings_.get<double>("wall_line_width_x");
    const Ratio wall_add_middle_threshold = std::max(1.0, std::min(99.0, 100.0 * min_odd_wall_line_width / wall_line_width_x)) / 100.0;

    const int wall_distribution_count = settings_.get<int>("wall_distribution_count");

    // === 新增功能：获取beading_strategy_scope参数 ===
    // 控制BeadingStrategy系统的应用范围
    EBeadingStrategyScope beading_strategy_scope = EBeadingStrategyScope::INNER_WALL_SKIN;  // 默认值
    try {
        beading_strategy_scope = settings_.get<EBeadingStrategyScope>("beading_strategy_scope");
    } catch (...) {
        // 参数未设置，使用默认值
        beading_strategy_scope = EBeadingStrategyScope::INNER_WALL_SKIN;
    }

    // === 核心功能：beading_strategy_scope控制 ===
    // 根据section_type和beading_strategy_scope决定是否使用BeadingStrategy
    bool should_use_beading_strategy = true;

    switch (beading_strategy_scope) {
        case EBeadingStrategyScope::OFF:
            should_use_beading_strategy = false;
            break;
        case EBeadingStrategyScope::ONLY_SKIN:
            should_use_beading_strategy = (section_type_ == SectionType::SKIN);
            break;
        case EBeadingStrategyScope::INNER_WALL_SKIN:
            // 对于skin墙体，全部使用BeadingStrategy
            // 对于普通墙体，如果只有外墙则使用简单偏移，如果有内墙则使用BeadingStrategy
            if (section_type_ == SectionType::SKIN) {
                should_use_beading_strategy = true;  // skin总是使用BeadingStrategy
            } else {
                should_use_beading_strategy = (inset_count_ > 1);  // 只有多层墙时才使用BeadingStrategy
            }
            break;
        case EBeadingStrategyScope::ALL:
        default:
            should_use_beading_strategy = true;
            break;
    }

    if (!should_use_beading_strategy) {
        // === 使用传统简单偏移算法 ===
        spdlog::debug("=== 使用传统简单偏移算法 ===");
        spdlog::debug("beading_strategy_scope={}, section_type={}, 使用简单偏移算法",
                     beading_strategy_scope == EBeadingStrategyScope::OFF ? "OFF" :
                     beading_strategy_scope == EBeadingStrategyScope::ONLY_SKIN ? "ONLY_SKIN" :
                     beading_strategy_scope == EBeadingStrategyScope::INNER_WALL_SKIN ? "INNER_WALL_SKIN" : "ALL",
                     section_type_ == SectionType::SKIN ? "SKIN" : "WALL");

        generateSimpleWalls(prepared_outline);
        return toolpaths_;
    }

    // === 原有BeadingStrategy路径 ===
    spdlog::debug("=== 使用BeadingStrategy系统 ===");
    spdlog::debug("beading_strategy_scope={}，启用复杂的线宽计算",
                 beading_strategy_scope == EBeadingStrategyScope::ALL ? "ALL" :
                 beading_strategy_scope == EBeadingStrategyScope::INNER_WALL_SKIN ? "INNER_WALL_SKIN" :
                 beading_strategy_scope == EBeadingStrategyScope::ONLY_SKIN ? "ONLY_SKIN" : "OFF");

    const size_t max_bead_count = (inset_count_ < std::numeric_limits<size_t>::max() / 2) ? 2 * inset_count_ : std::numeric_limits<size_t>::max();
    const auto beading_strat = BeadingStrategyFactory::makeStrategy(
        bead_width_0_,
        bead_width_x_,
        wall_transition_length,
        transitioning_angle,
        print_thin_walls_,
        safe_min_bead_width,  // 使用修复后的安全值
        min_feature_size_,
        wall_split_middle_threshold,
        wall_add_middle_threshold,
        max_bead_count,
        wall_0_inset_,
        wall_distribution_count);
    const auto transition_filter_dist = settings_.get<coord_t>("wall_transition_filter_distance");
    const auto allowed_filter_deviation = settings_.get<coord_t>("wall_transition_filter_deviation");
    SkeletalTrapezoidation wall_maker(
        prepared_outline,
        *beading_strat,
        beading_strat->getTransitioningAngle(),
        discretization_step_size,
        transition_filter_dist,
        allowed_filter_deviation,
        wall_transition_length,
        layer_idx_,
        section_type_);
    wall_maker.generateToolpaths(toolpaths_);
    scripta::log(
        "toolpaths_0",
        toolpaths_,
        section_type_,
        layer_idx_,
        scripta::CellVDI{ "is_closed", &ExtrusionLine::is_closed_ },
        scripta::CellVDI{ "is_odd", &ExtrusionLine::is_odd_ },
        scripta::CellVDI{ "inset_idx", &ExtrusionLine::inset_idx_ },
        scripta::PointVDI{ "width", &ExtrusionJunction::w_ },
        scripta::PointVDI{ "perimeter_index", &ExtrusionJunction::perimeter_index_ });

    stitchToolPaths(toolpaths_, settings_);
    scripta::log(
        "toolpaths_1",
        toolpaths_,
        section_type_,
        layer_idx_,
        scripta::CellVDI{ "is_closed", &ExtrusionLine::is_closed_ },
        scripta::CellVDI{ "is_odd", &ExtrusionLine::is_odd_ },
        scripta::CellVDI{ "inset_idx", &ExtrusionLine::inset_idx_ },
        scripta::PointVDI{ "width", &ExtrusionJunction::w_ },
        scripta::PointVDI{ "perimeter_index", &ExtrusionJunction::perimeter_index_ });

    removeSmallFillLines(toolpaths_);
    scripta::log(
        "toolpaths_2",
        toolpaths_,
        section_type_,
        layer_idx_,
        scripta::CellVDI{ "is_closed", &ExtrusionLine::is_closed_ },
        scripta::CellVDI{ "is_odd", &ExtrusionLine::is_odd_ },
        scripta::CellVDI{ "inset_idx", &ExtrusionLine::inset_idx_ },
        scripta::PointVDI{ "width", &ExtrusionJunction::w_ },
        scripta::PointVDI{ "perimeter_index", &ExtrusionJunction::perimeter_index_ });

    simplifyToolPaths(toolpaths_, settings_);
    scripta::log(
        "toolpaths_3",
        toolpaths_,
        section_type_,
        layer_idx_,
        scripta::CellVDI{ "is_closed", &ExtrusionLine::is_closed_ },
        scripta::CellVDI{ "is_odd", &ExtrusionLine::is_odd_ },
        scripta::CellVDI{ "inset_idx", &ExtrusionLine::inset_idx_ },
        scripta::PointVDI{ "width", &ExtrusionJunction::w_ },
        scripta::PointVDI{ "perimeter_index", &ExtrusionJunction::perimeter_index_ });

    separateOutInnerContour();

    removeEmptyToolPaths(toolpaths_);
    scripta::log(
        "toolpaths_4",
        toolpaths_,
        section_type_,
        layer_idx_,
        scripta::CellVDI{ "is_closed", &ExtrusionLine::is_closed_ },
        scripta::CellVDI{ "is_odd", &ExtrusionLine::is_odd_ },
        scripta::CellVDI{ "inset_idx", &ExtrusionLine::inset_idx_ },
        scripta::PointVDI{ "width", &ExtrusionJunction::w_ },
        scripta::PointVDI{ "perimeter_index", &ExtrusionJunction::perimeter_index_ });
    assert(
        std::is_sorted(
            toolpaths_.cbegin(),
            toolpaths_.cend(),
            [](const VariableWidthLines& l, const VariableWidthLines& r)
            {
                return l.front().inset_idx_ < r.front().inset_idx_;
            })
        && "WallToolPaths should be sorted from the outer 0th to inner_walls");
    toolpaths_generated_ = true;
    scripta::log(
        "toolpaths_5",
        toolpaths_,
        section_type_,
        layer_idx_,
        scripta::CellVDI{ "is_closed", &ExtrusionLine::is_closed_ },
        scripta::CellVDI{ "is_odd", &ExtrusionLine::is_odd_ },
        scripta::CellVDI{ "inset_idx", &ExtrusionLine::inset_idx_ },
        scripta::PointVDI{ "width", &ExtrusionJunction::w_ },
        scripta::PointVDI{ "perimeter_index", &ExtrusionJunction::perimeter_index_ });
    return toolpaths_;
}


void WallToolPaths::stitchToolPaths(std::vector<VariableWidthLines>& toolpaths, const Settings& settings)
{
    const coord_t stitch_distance
        = settings.get<coord_t>("wall_line_width_x") - 1; // In 0-width contours, junctions can cause up to 1-line-width gaps. Don't stitch more than 1 line width.

    for (unsigned int wall_idx = 0; wall_idx < toolpaths.size(); wall_idx++)
    {
        VariableWidthLines& wall_lines = toolpaths[wall_idx];

        VariableWidthLines stitched_polylines;
        VariableWidthLines closed_polygons;
        ExtrusionLineStitcher::stitch(wall_lines, stitched_polylines, closed_polygons, stitch_distance);
        wall_lines = stitched_polylines; // replace input toolpaths with stitched polylines

        for (ExtrusionLine& wall_polygon : closed_polygons)
        {
            if (wall_polygon.junctions_.empty())
            {
                continue;
            }
            wall_polygon.is_closed_ = true;
            wall_lines.emplace_back(std::move(wall_polygon)); // add stitched polygons to result
        }
#ifdef DEBUG
        for (ExtrusionLine& line : wall_lines)
        {
            assert(line.inset_idx_ == wall_idx);
        }
#endif // DEBUG
    }
}

void WallToolPaths::generateSimpleWalls(const Shape& outline)
{
    // === 传统简单偏移算法实现 ===
    // 完全绕过BeadingStrategy系统，使用固定线宽的简单偏移

    spdlog::debug("=== 开始传统简单偏移算法 ===");
    spdlog::debug("目标墙数: {}, 外墙线宽: {}, 内墙线宽: {}", inset_count_, bead_width_0_, bead_width_x_);

    toolpaths_.clear();
    toolpaths_.resize(inset_count_);

    Shape current_outline = outline;

    // 生成每一层墙
    for (size_t wall_idx = 0; wall_idx < inset_count_; wall_idx++)
    {
        if (current_outline.empty())
        {
            spdlog::debug("第{}层墙：轮廓为空，停止生成", wall_idx);
            break;
        }

        // 确定当前墙的线宽
        coord_t current_line_width = (wall_idx == 0) ? bead_width_0_ : bead_width_x_;

        // 计算偏移距离
        coord_t offset_distance = current_line_width / 2;
        if (wall_idx == 0 && wall_0_inset_ > 0)
        {
            offset_distance += wall_0_inset_;  // 外墙额外内缩
        }

        spdlog::debug("第{}层墙：线宽={}, 偏移距离={}", wall_idx, current_line_width, offset_distance);

        // === 修复：先计算偏移后的轮廓，再创建ExtrusionLine ===
        // 计算当前层墙的偏移轮廓
        Shape offset_outline = current_outline.offset(-offset_distance);

        // 为偏移后轮廓的每个多边形创建ExtrusionLine
        for (const auto& offset_polygon : offset_outline)
        {
            if (offset_polygon.size() < 3) continue;  // 跳过无效多边形

            // === 新策略：在多边形初始化时插入Z接缝插值点 ===
            // 只对外轮廓（wall_idx == 0）进行插值点插入
            Polygon processed_polygon = offset_polygon;
            if (wall_idx == 0)
            {
                // 获取正确的层Z坐标：优先使用传入的layer_z_，否则回退到简单计算
                coord_t layer_z;
                if (layer_z_ >= 0)
                {
                    // 使用传入的正确Z坐标（支持可变层厚）
                    layer_z = layer_z_;
                }
                else
                {
                    // 回退到简单计算（向后兼容）
                    layer_z = layer_idx_ * settings_.get<coord_t>("layer_height");
                }

                processed_polygon = insertZSeamInterpolationPoints(offset_polygon, settings_, layer_z);

                if (processed_polygon.size() != offset_polygon.size())
                {
                    spdlog::debug("外轮廓插值点插入成功：顶点数 {} -> {}，使用Z坐标: {:.2f}mm",
                               offset_polygon.size(), processed_polygon.size(), INT2MM(layer_z));
                }
            }

            ExtrusionLine wall_line(wall_idx, false);  // inset_idx, is_odd

            // 将偏移后多边形的每个点转换为ExtrusionJunction
            for (size_t point_idx = 0; point_idx < processed_polygon.size(); point_idx++)
            {
                ExtrusionJunction junction(processed_polygon[point_idx], current_line_width, wall_idx);
                wall_line.junctions_.emplace_back(junction);
            }

            // 闭合多边形
            wall_line.is_closed_ = true;

            if (!wall_line.junctions_.empty())
            {
                toolpaths_[wall_idx].emplace_back(std::move(wall_line));
            }
        }

        // 为下一层墙计算新的轮廓（向内偏移）
        // 修复：应该基于当前轮廓偏移整个线宽，而不是基于已偏移的轮廓再次偏移
        current_outline = current_outline.offset(-current_line_width);

        spdlog::debug("第{}层墙生成完成，剩余轮廓多边形数: {}", wall_idx, current_outline.size());
    }

    // 设置内部轮廓（最后一层偏移的结果）
    inner_contour_ = current_outline;

    // 标记工具路径已生成
    toolpaths_generated_ = true;

    spdlog::debug("=== 传统简单偏移算法完成 ===");
    spdlog::debug("成功生成{}层墙，内部轮廓多边形数: {}", inset_count_, inner_contour_.size());

    // 统计生成的路径数量
    size_t total_lines = 0;
    for (size_t i = 0; i < toolpaths_.size(); i++)
    {
        total_lines += toolpaths_[i].size();
        spdlog::debug("第{}层墙包含{}条路径", i, toolpaths_[i].size());
    }
    spdlog::debug("总共生成{}条打印路径", total_lines);
}

void WallToolPaths::removeSmallFillLines(std::vector<VariableWidthLines>& toolpaths)
{
    for (VariableWidthLines& inset : toolpaths)
    {
        for (size_t line_idx = 0; line_idx < inset.size(); line_idx++)
        {
            ExtrusionLine& line = inset[line_idx];
            if (line.is_outer_wall())
            {
                continue;
            }
            coord_t min_width = std::numeric_limits<coord_t>::max();
            for (const ExtrusionJunction& j : line)
            {
                min_width = std::min(min_width, j.w_);
            }
            if (line.is_odd_ && ! line.is_closed_ && line.shorterThan(min_width / 2))
            { // remove line
                line = std::move(inset.back());
                inset.erase(--inset.end());
                line_idx--; // reconsider the current position
            }
        }
    }
}

void WallToolPaths::simplifyToolPaths(std::vector<VariableWidthLines>& toolpaths, const Settings& settings)
{
    const Simplify simplifier(settings);
    for (auto& toolpath : toolpaths)
    {
        toolpath = toolpath
                 | ranges::views::transform(
                       [&simplifier](auto& line)
                       {
                           auto line_ = line.is_closed_ ? simplifier.polygon(line) : simplifier.polyline(line);

                           if (line_.is_closed_ && line_.size() >= 2 && line_.front() != line_.back())
                           {
                               line_.emplace_back(line_.front());
                           }
                           return line_;
                       })
                 | ranges::views::filter(
                       [](const auto& line)
                       {
                           return ! line.empty();
                       })
                 | ranges::to_vector;
    }
}

const std::vector<VariableWidthLines>& WallToolPaths::getToolPaths()
{
    if (! toolpaths_generated_)
    {
        return generate();
    }
    return toolpaths_;
}

void WallToolPaths::pushToolPaths(std::vector<VariableWidthLines>& paths)
{
    if (! toolpaths_generated_)
    {
        generate();
    }
    paths.insert(paths.end(), toolpaths_.begin(), toolpaths_.end());
}

void WallToolPaths::separateOutInnerContour()
{
    // We'll remove all 0-width paths from the original toolpaths and store them separately as polygons.
    std::vector<VariableWidthLines> actual_toolpaths;
    actual_toolpaths.reserve(toolpaths_.size()); // A bit too much, but the correct order of magnitude.
    std::vector<VariableWidthLines> contour_paths;
    contour_paths.reserve(toolpaths_.size() / inset_count_);
    inner_contour_.clear();
    for (const VariableWidthLines& inset : toolpaths_)
    {
        if (inset.empty())
        {
            continue;
        }
        bool is_contour = false;
        for (const ExtrusionLine& line : inset)
        {
            for (const ExtrusionJunction& j : line)
            {
                if (j.w_ == 0)
                {
                    is_contour = true;
                }
                else
                {
                    is_contour = false;
                }
                break;
            }
        }


        if (is_contour)
        {
#ifdef DEBUG
            for (const ExtrusionLine& line : inset)
            {
                for (const ExtrusionJunction& j : line)
                {
                    assert(j.w_ == 0);
                }
            }
#endif // DEBUG
            for (const ExtrusionLine& line : inset)
            {
                if (line.is_odd_)
                {
                    continue; // odd lines don't contribute to the contour
                }
                else if (line.is_closed_) // sometimes an very small even polygonal wall is not stitched into a polygon
                {
                    inner_contour_.emplace_back(line.toPolygon());
                }
            }
        }
        else
        {
            actual_toolpaths.emplace_back(inset);
        }
    }
    if (! actual_toolpaths.empty())
    {
        toolpaths_ = std::move(actual_toolpaths); // Filtered out the 0-width paths.
    }
    else
    {
        toolpaths_.clear();
    }

    // The output walls from the skeletal trapezoidation have no known winding order, especially if they are joined together from polylines.
    // They can be in any direction, clockwise or counter-clockwise, regardless of whether the shapes are positive or negative.
    // To get a correct shape, we need to make the outside contour positive and any holes inside negative.
    // This can be done by applying the even-odd rule to the shape. This rule is not sensitive to the winding order of the polygon.
    // The even-odd rule would be incorrect if the polygon self-intersects, but that should never be generated by the skeletal trapezoidation.
    inner_contour_ = inner_contour_.processEvenOdd();
}

const Shape& WallToolPaths::getInnerContour()
{
    if (! toolpaths_generated_ && inset_count_ > 0)
    {
        generate();
    }
    else if (inset_count_ == 0)
    {
        return outline_;
    }
    return inner_contour_;
}

bool WallToolPaths::removeEmptyToolPaths(std::vector<VariableWidthLines>& toolpaths)
{
    for (VariableWidthLines& toolpath : toolpaths)
    {
        toolpath.erase(
            std::remove_if(
                toolpath.begin(),
                toolpath.end(),
                [](const ExtrusionLine& line)
                {
                    return line.junctions_.empty();
                }),
            toolpath.end());
    }

    toolpaths.erase(
        std::remove_if(
            toolpaths.begin(),
            toolpaths.end(),
            [](const VariableWidthLines& lines)
            {
                return lines.empty();
            }),
        toolpaths.end());
    return toolpaths.empty();
}

Polygon WallToolPaths::insertZSeamInterpolationPoints(const Polygon& polygon, const Settings& settings, coord_t layer_z)
{
    // 检查是否启用自定义Z接缝点功能
    if (!settings.get<bool>("draw_z_seam_enable"))
    {
        return polygon; // 功能未启用，返回原多边形
    }

    // 检查是否启用插值功能
    if (!settings.get<bool>("z_seam_point_interpolation"))
    {
        return polygon; // 插值功能未启用，返回原多边形
    }

    // 获取Z接缝点列表
    auto z_seam_points = settings.get<std::vector<Point3LL>>("draw_z_seam_points");
    if (z_seam_points.empty())
    {
        return polygon; // 没有设置接缝点，返回原多边形
    }

    spdlog::debug("=== Z接缝点插值预处理开始 ===");
    spdlog::debug("当前层Z坐标: {:.2f}mm, 多边形顶点数: {}", INT2MM(layer_z), polygon.size());

    // 创建ZSeamConfig进行插值计算
    ZSeamConfig temp_config;
    temp_config.draw_z_seam_enable_ = true;
    temp_config.draw_z_seam_points_ = z_seam_points;
    temp_config.z_seam_point_interpolation_ = true;
    temp_config.draw_z_seam_grow_ = settings.get<bool>("draw_z_seam_grow");
    temp_config.current_layer_z_ = layer_z;

    // 尝试获取插值位置
    auto interpolated_pos = temp_config.getInterpolatedSeamPosition();
    if (!interpolated_pos.has_value())
    {
        spdlog::debug("插值计算失败，返回原多边形");
        return polygon;
    }

    Point2LL target_point = interpolated_pos.value();
    //CURA_INFO("插值目标点: ({:.2f}, {:.2f})", INT2MM(target_point.X), INT2MM(target_point.Y));

    // 在多边形中查找最近的线段并插入插值点
    const PointsSet& points = polygon;
    if (points.size() < 3)
    {
        spdlog::debug("多边形顶点数不足，返回原多边形");
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

    spdlog::debug("最近线段: 索引{}, 距离: {:.2f}mm", best_segment_idx, INT2MM(std::sqrt(min_distance_sq)));

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

        spdlog::debug("在索引{}插入新点: ({:.2f}, {:.2f})",
                    insert_idx, INT2MM(closest_point_on_segment.X), INT2MM(closest_point_on_segment.Y));
        spdlog::debug("多边形顶点数: {} -> {}", polygon.size(), result_polygon.size());

        return result_polygon;
    }
    else
    {
        spdlog::debug("最近点是现有顶点，无需插入新点");
        return polygon;
    }
}

} // namespace cura
