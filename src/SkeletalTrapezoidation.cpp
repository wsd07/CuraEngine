// Copyright (c) 2023 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#include "SkeletalTrapezoidation.h"

#include <numbers>
#include <queue>
#include <sstream>
#include <stack>
#include <unordered_set>

#include <scripta/logger.h>
#include <spdlog/spdlog.h>

#include "BoostInterface.hpp"
#include "settings/types/Ratio.h"
#include "utils/VoronoiUtils.h"
#include "utils/linearAlg2D.h"
#include "utils/macros.h"
#include "utils/polygonUtils.h"
#include "utils/DebugManager.h"
#include "utils/CrashSafeDebug.h"

#define SKELETAL_TRAPEZOIDATION_BEAD_SEARCH_MAX \
    1000 // A limit to how long it'll keep searching for adjacent beads. Increasing will re-use beadings more often (saving performance), but search longer for beading (costing
         // performance).

namespace cura
{

SkeletalTrapezoidation::node_t& SkeletalTrapezoidation::makeNode(vd_t::vertex_type& vd_node, Point2LL p)
{
    auto he_node_it = vd_node_to_he_node_.find(&vd_node);
    if (he_node_it == vd_node_to_he_node_.end())
    {
        graph_.nodes.emplace_front(SkeletalTrapezoidationJoint(), p);
        node_t& node = graph_.nodes.front();
        vd_node_to_he_node_.emplace(&vd_node, &node);
        return node;
    }
    else
    {
        return *he_node_it->second;
    }
}

void SkeletalTrapezoidation::transferEdge(
    Point2LL from,
    Point2LL to,
    vd_t::edge_type& vd_edge,
    edge_t*& prev_edge,
    Point2LL& start_source_point,
    Point2LL& end_source_point,
    const std::vector<Point2LL>& points,
    const std::vector<Segment>& segments)
{
    auto he_edge_it = vd_edge_to_he_edge_.find(vd_edge.twin());
    if (he_edge_it != vd_edge_to_he_edge_.end())
    { // Twin segment(s) have already been made
        edge_t* source_twin = he_edge_it->second;
        // 使用崩溃安全断言，确保错误信息能够输出
        if (!source_twin) {
            CURA_ERROR_FLUSH_F("source_twin为空指针，Voronoi边缘映射失败。vd_edge地址=%p, twin地址=%p, 映射表大小=%zu",
                              static_cast<const void*>(&vd_edge),
                              static_cast<const void*>(vd_edge.twin()),
                              vd_edge_to_he_edge_.size());
            CURA_ASSERT_WITH_INFO(false, "source_twin为空指针，程序将崩溃以便调试");
        }

        auto end_node_it = vd_node_to_he_node_.find(vd_edge.vertex1());
        // assert(end_node_it != vd_node_to_he_node_.end()); // 原始断言，注释保留以备回滚
        if (end_node_it == vd_node_to_he_node_.end())
        {
            CURA_ERROR("BeadingStrategy错误: 无法找到Voronoi顶点对应的半边节点");
            CURA_ERROR("  - vertex1地址: {}", static_cast<const void*>(vd_edge.vertex1()));
            CURA_ERROR("  - vd_node_to_he_node_映射大小: {}", vd_node_to_he_node_.size());
            return;
        }
        node_t* end_node = end_node_it->second;
        for (edge_t* twin = source_twin;; twin = twin->prev_->twin_->prev_)
        {
            if (! twin)
            {
                CURA_ERROR("BeadingStrategy错误: 遇到没有twin的Voronoi边");
                CURA_ERROR("  - 当前循环位置: source_twin={}", static_cast<const void*>(source_twin));
                CURA_ERROR("  - 可能原因: Voronoi图构建不完整或边缘数据损坏");
                continue; // Prevent reading unallocated memory.
            }
            // assert(twin); // 原始断言，注释保留以备回滚 - 上面已经检查过了
            graph_.edges.emplace_front(SkeletalTrapezoidationEdge());
            edge_t* edge = &graph_.edges.front();
            edge->from_ = twin->to_;
            edge->to_ = twin->from_;
            edge->twin_ = twin;
            twin->twin_ = edge;
            edge->from_->incident_edge_ = edge;

            if (prev_edge)
            {
                edge->prev_ = prev_edge;
                prev_edge->next_ = edge;
            }

            prev_edge = edge;

            if (prev_edge->to_ == end_node)
            {
                return;
            }

            if (! twin->prev_ || ! twin->prev_->twin_ || ! twin->prev_->twin_->prev_)
            {
                CURA_ERROR("BeadingStrategy错误: 离散化段表现异常");
                CURA_ERROR("  - twin->prev_: {}", static_cast<const void*>(twin->prev_));
                CURA_ERROR("  - twin->prev_->twin_: {}", twin->prev_ ? static_cast<const void*>(twin->prev_->twin_) : nullptr);
                CURA_ERROR("  - twin->prev_->twin_->prev_: {}", (twin->prev_ && twin->prev_->twin_) ? static_cast<const void*>(twin->prev_->twin_->prev_) : nullptr);
                CURA_ERROR("  - 可能原因: Voronoi图拓扑结构不完整");
                return;
            }

            // assert(twin->prev_); // Forth rib - 原始断言，注释保留以备回滚
            // assert(twin->prev_->twin_); // Back rib - 原始断言，注释保留以备回滚
            // assert(twin->prev_->twin_->prev_); // Prev segment along parabola - 原始断言，注释保留以备回滚
            graph_.makeRib(prev_edge, start_source_point, end_source_point);
        }
        // assert(prev_edge); // 原始断言，注释保留以备回滚
        if (!prev_edge)
        {
            CURA_ERROR("BeadingStrategy错误: prev_edge为空，边缘传输失败");
            CURA_ERROR("  - source_twin: {}", static_cast<const void*>(source_twin));
            CURA_ERROR("  - end_node: {}", static_cast<const void*>(end_node));
            return;
        }
    }
    else
    {
        std::vector<Point2LL> discretized = discretize(vd_edge, points, segments);
        // assert(discretized.size() >= 2); // 原始断言，注释保留以备回滚
        if (discretized.size() < 2)
        {
            CURA_ERROR("BeadingStrategy错误: 离散化Voronoi边退化");
            CURA_ERROR("  - discretized.size(): {}", discretized.size());
            CURA_ERROR("  - vd_edge是否有限: {}", vd_edge.is_finite());
            CURA_ERROR("  - 可能原因: 边缘太短或几何计算精度问题");
            return;
        }

        // assert(! prev_edge || prev_edge->to_); // 原始断言，注释保留以备回滚
        if (prev_edge && ! prev_edge->to_)
        {
            CURA_ERROR("BeadingStrategy错误: 前一条边没有终点");
            CURA_ERROR("  - prev_edge: {}", static_cast<const void*>(prev_edge));
            CURA_ERROR("  - prev_edge->from_: {}", static_cast<const void*>(prev_edge->from_));
            CURA_ERROR("  - 可能原因: 边缘构建过程中的拓扑错误");
            return;
        }
        node_t* v0
            = (prev_edge) ? prev_edge->to_ : &makeNode(*vd_edge.vertex0(), from); // TODO: investigate whether boost:voronoi can produce multiple verts and violates consistency
        Point2LL p0 = discretized.front();
        for (size_t p1_idx = 1; p1_idx < discretized.size(); p1_idx++)
        {
            Point2LL p1 = discretized[p1_idx];
            node_t* v1;
            if (p1_idx < discretized.size() - 1)
            {
                graph_.nodes.emplace_front(SkeletalTrapezoidationJoint(), p1);
                v1 = &graph_.nodes.front();
            }
            else
            {
                v1 = &makeNode(*vd_edge.vertex1(), to);
            }

            graph_.edges.emplace_front(SkeletalTrapezoidationEdge());
            edge_t* edge = &graph_.edges.front();
            edge->from_ = v0;
            edge->to_ = v1;
            edge->from_->incident_edge_ = edge;

            if (prev_edge)
            {
                edge->prev_ = prev_edge;
                prev_edge->next_ = edge;
            }

            prev_edge = edge;
            p0 = p1;
            v0 = v1;

            if (p1_idx < discretized.size() - 1)
            { // Rib for last segment gets introduced outside this function!
                graph_.makeRib(prev_edge, start_source_point, end_source_point);
            }
        }
        // assert(prev_edge); // 原始断言，注释保留以备回滚
        if (!prev_edge)
        {
            CURA_ERROR("BeadingStrategy错误: 离散化后prev_edge为空");
            CURA_ERROR("  - discretized.size(): {}", discretized.size());
            CURA_ERROR("  - 可能原因: 边缘创建过程失败");
            return;
        }
        vd_edge_to_he_edge_.emplace(&vd_edge, prev_edge);
    }
}

std::vector<Point2LL> SkeletalTrapezoidation::discretize(const vd_t::edge_type& vd_edge, const std::vector<Point2LL>& points, const std::vector<Segment>& segments)
{
    /*Terminology in this function assumes that the edge moves horizontally from
    left to right. This is not necessarily the case; the edge can go in any
    direction, but it helps to picture it in a certain direction in your head.*/

    const vd_t::cell_type* left_cell = vd_edge.cell();
    const vd_t::cell_type* right_cell = vd_edge.twin()->cell();
    Point2LL start = VoronoiUtils::p(vd_edge.vertex0());
    Point2LL end = VoronoiUtils::p(vd_edge.vertex1());

    bool point_left = left_cell->contains_point();
    bool point_right = right_cell->contains_point();
    if ((! point_left && ! point_right) || vd_edge.is_secondary()) // Source vert is directly connected to source segment
    {
        return std::vector<Point2LL>({ start, end });
    }
    else if (point_left != point_right) // This is a parabolic edge between a point and a line.
    {
        Point2LL p = VoronoiUtils::getSourcePoint(*(point_left ? left_cell : right_cell), points, segments);
        const Segment& s = VoronoiUtils::getSourceSegment(*(point_left ? right_cell : left_cell), points, segments);
        return VoronoiUtils::discretizeParabola(p, s, start, end, discretization_step_size_, transitioning_angle_);
    }
    else // This is a straight edge between two points.
    {
        /*While the edge is straight, it is still discretized since the part
        becomes narrower between the two points. As such it may need different
        beadings along the way.*/
        Point2LL left_point = VoronoiUtils::getSourcePoint(*left_cell, points, segments);
        Point2LL right_point = VoronoiUtils::getSourcePoint(*right_cell, points, segments);
        coord_t d = vSize(right_point - left_point);
        Point2LL middle = (left_point + right_point) / 2;
        Point2LL x_axis_dir = turn90CCW(right_point - left_point);
        coord_t x_axis_length = vSize(x_axis_dir);

        const auto projected_x = [x_axis_dir, x_axis_length, middle](Point2LL from) // Project a point on the edge.
        {
            Point2LL vec = from - middle;
            coord_t x = dot(vec, x_axis_dir) / x_axis_length;
            return x;
        };

        coord_t start_x = projected_x(start);
        coord_t end_x = projected_x(end);

        // Part of the edge will be bound to the markings on the endpoints of the edge. Calculate how far that is.
        double bound = 0.5 / tan((std::numbers::pi - transitioning_angle_) * 0.5);
        coord_t marking_start_x = -d * bound;
        coord_t marking_end_x = d * bound;
        Point2LL marking_start = middle + x_axis_dir * marking_start_x / x_axis_length;
        Point2LL marking_end = middle + x_axis_dir * marking_end_x / x_axis_length;
        int direction = 1;

        if (start_x > end_x) // Oops, the Voronoi edge is the other way around.
        {
            direction = -1;
            std::swap(marking_start, marking_end);
            std::swap(marking_start_x, marking_end_x);
        }

        // Start generating points along the edge.
        Point2LL a = start;
        Point2LL b = end;
        std::vector<Point2LL> ret;
        ret.emplace_back(a);

        // Introduce an extra edge at the borders of the markings?
        bool add_marking_start = marking_start_x * direction > start_x * direction;
        bool add_marking_end = marking_end_x * direction > start_x * direction;

        // The edge's length may not be divisible by the step size, so calculate an integer step count and evenly distribute the vertices among those.
        Point2LL ab = b - a;
        coord_t ab_size = vSize(ab);
        coord_t step_count = (ab_size + discretization_step_size_ / 2) / discretization_step_size_;
        if (step_count % 2 == 1)
        {
            step_count++; // enforce a discretization point being added in the middle
        }
        for (coord_t step = 1; step < step_count; step++)
        {
            Point2LL here = a + ab * step / step_count; // Now simply interpolate the coordinates to get the new vertices!
            coord_t x_here = projected_x(here); // If we've surpassed the position of the extra markings, we may need to insert them first.
            if (add_marking_start && marking_start_x * direction < x_here * direction)
            {
                ret.emplace_back(marking_start);
                add_marking_start = false;
            }
            if (add_marking_end && marking_end_x * direction < x_here * direction)
            {
                ret.emplace_back(marking_end);
                add_marking_end = false;
            }
            ret.emplace_back(here);
        }
        if (add_marking_end && marking_end_x * direction < end_x * direction)
        {
            ret.emplace_back(marking_end);
        }
        ret.emplace_back(b);
        return ret;
    }
}


bool SkeletalTrapezoidation::computePointCellRange(
    vd_t::cell_type& cell,
    Point2LL& start_source_point,
    Point2LL& end_source_point,
    vd_t::edge_type*& starting_vd_edge,
    vd_t::edge_type*& ending_vd_edge,
    const std::vector<Point2LL>& points,
    const std::vector<Segment>& segments)
{
    if (cell.incident_edge()->is_infinite())
    {
        return false; // Infinite edges only occur outside of the polygon. Don't copy any part of this cell.
    }
    // Check if any point of the cell is inside or outside polygon
    // Copy whole cell into graph or not at all

    const Point2LL source_point = VoronoiUtils::getSourcePoint(cell, points, segments);
    const PolygonsPointIndex source_point_index = VoronoiUtils::getSourcePointIndex(cell, points, segments);
    Point2LL some_point = VoronoiUtils::p(cell.incident_edge()->vertex0());
    if (some_point == source_point)
    {
        some_point = VoronoiUtils::p(cell.incident_edge()->vertex1());
    }
    // Test if the some_point is even inside the polygon.
    // The edge leading out of a polygon must have an endpoint that's not in the corner following the contour of the polygon at that vertex.
    // So if it's inside the corner formed by the polygon vertex, it's all fine.
    // But if it's outside of the corner, it must be a vertex of the Voronoi diagram that goes outside of the polygon towards infinity.
    if (! LinearAlg2D::isInsideCorner(source_point_index.prev().p(), source_point_index.p(), source_point_index.next().p(), some_point))
    {
        return false; // Don't copy any part of this cell
    }
    vd_t::edge_type* vd_edge = cell.incident_edge();
    do
    {
        // assert(vd_edge->is_finite()); // 原始断言，注释保留以备回滚
        if (!vd_edge->is_finite())
        {
            CURA_ERROR("BeadingStrategy错误: 点cell中遇到无限边");
            CURA_ERROR("  - source_point: ({}, {})", source_point.X, source_point.Y);
            CURA_ERROR("  - 可能原因: Voronoi图构建时包含了无限边，这在点cell中不应该发生");
            return false;
        }

        Point2LL p1 = VoronoiUtils::p(vd_edge->vertex1());
        if (p1 == source_point)
        {
            start_source_point = source_point;
            end_source_point = source_point;
            starting_vd_edge = vd_edge->next();
            ending_vd_edge = vd_edge;
        }
        else
        {
            // 使用崩溃安全断言检查点cell拓扑
            Point2LL p0 = VoronoiUtils::p(vd_edge->vertex0());
            if (!(p0 == source_point || !vd_edge->is_secondary()))
            {
                CURA_ERROR_FLUSH_F("点cell拓扑错误: source=(%d,%d), vertex0=(%d,%d), vertex1=(%d,%d), is_secondary=%s",
                                  source_point.X, source_point.Y, p0.X, p0.Y, p1.X, p1.Y,
                                  vd_edge->is_secondary() ? "true" : "false");
                CURA_ERROR_FLUSH("原因: 点cell不能用边穿过点，因为输入中不允许共线边");
                CURA_ASSERT_WITH_INFO(false, "点cell拓扑验证失败，程序将崩溃以便调试");
            }
        }
    } while (vd_edge = vd_edge->next(), vd_edge != cell.incident_edge());

    // assert(starting_vd_edge && ending_vd_edge); // 原始断言，注释保留以备回滚
    if (!starting_vd_edge || !ending_vd_edge)
    {
        CURA_ERROR("BeadingStrategy错误: 无法找到点cell的起始或结束边");
        CURA_ERROR("  - starting_vd_edge: {}", static_cast<const void*>(starting_vd_edge));
        CURA_ERROR("  - ending_vd_edge: {}", static_cast<const void*>(ending_vd_edge));
        CURA_ERROR("  - source_point: ({}, {})", source_point.X, source_point.Y);
        return false;
    }

    // assert(starting_vd_edge != ending_vd_edge); // 原始断言，注释保留以备回滚
    if (starting_vd_edge == ending_vd_edge)
    {
        CURA_ERROR("BeadingStrategy错误: 起始边和结束边相同");
        CURA_ERROR("  - edge地址: {}", static_cast<const void*>(starting_vd_edge));
        CURA_ERROR("  - source_point: ({}, {})", source_point.X, source_point.Y);
        CURA_ERROR("  - 可能原因: 点cell只有一条边，这在正常情况下不应该发生");
        return false;
    }
    return true;
}

void SkeletalTrapezoidation::computeSegmentCellRange(
    vd_t::cell_type& cell,
    Point2LL& start_source_point,
    Point2LL& end_source_point,
    vd_t::edge_type*& starting_vd_edge,
    vd_t::edge_type*& ending_vd_edge,
    const std::vector<Point2LL>& points,
    const std::vector<Segment>& segments)
{
    const Segment& source_segment = VoronoiUtils::getSourceSegment(cell, points, segments);
    Point2LL from = source_segment.from();
    Point2LL to = source_segment.to();

    // Find starting edge
    // Find end edge
    bool seen_possible_start = false;
    bool after_start = false;
    bool ending_edge_is_set_before_start = false;
    vd_t::edge_type* edge = cell.incident_edge();
    do
    {
        if (edge->is_infinite())
        {
            continue;
        }
        Point2LL v0 = VoronoiUtils::p(edge->vertex0());
        Point2LL v1 = VoronoiUtils::p(edge->vertex1());
        // assert(! (v0 == to && v1 == from)); // 原始断言，注释保留以备回滚
        if (v0 == to && v1 == from)
        {
            CURA_ERROR("BeadingStrategy错误: segment cell中发现反向边");
            CURA_ERROR("  - segment: from({}, {}) to({}, {})", from.X, from.Y, to.X, to.Y);
            CURA_ERROR("  - edge: v0({}, {}) v1({}, {})", v0.X, v0.Y, v1.X, v1.Y);
            CURA_ERROR("  - 可能原因: Voronoi边的方向与segment方向相反，这可能导致拓扑错误");
            continue; // 跳过这条边，继续处理其他边
        }
        if (v0 == to && ! after_start) // Use the last edge which starts in source_segment.to
        {
            starting_vd_edge = edge;
            seen_possible_start = true;
        }
        else if (seen_possible_start)
        {
            after_start = true;
        }

        if (v1 == from && (! ending_vd_edge || ending_edge_is_set_before_start))
        {
            ending_edge_is_set_before_start = ! after_start;
            ending_vd_edge = edge;
        }
    } while (edge = edge->next(), edge != cell.incident_edge());

    // 检查是否找到了有效的边缘
    if (!starting_vd_edge || !ending_vd_edge)
    {
        spdlog::debug("computeSegmentCellRange: 无法找到有效的边缘，source_segment: from({}, {}) to({}, {})",
                     from.X, from.Y, to.X, to.Y);
        throw std::runtime_error("无法找到有效的Voronoi边缘");
    }

    // assert(starting_vd_edge && ending_vd_edge); // 原始断言，注释保留以备回滚
    if (!starting_vd_edge || !ending_vd_edge)
    {
        CURA_ERROR("BeadingStrategy错误: segment cell无法找到有效的起始或结束边");
        CURA_ERROR("  - starting_vd_edge: {}", static_cast<const void*>(starting_vd_edge));
        CURA_ERROR("  - ending_vd_edge: {}", static_cast<const void*>(ending_vd_edge));
        CURA_ERROR("  - segment: from({}, {}) to({}, {})", from.X, from.Y, to.X, to.Y);
        CURA_ERROR("  - seen_possible_start: {}, after_start: {}", seen_possible_start, after_start);
        throw std::runtime_error("无法找到有效的Voronoi边缘");
    }

    if (starting_vd_edge == ending_vd_edge)
    {
        CURA_ERROR("BeadingStrategy警告: segment cell的起始边和结束边相同");
        CURA_ERROR("  - edge地址: {}", static_cast<const void*>(starting_vd_edge));
        CURA_ERROR("  - segment: from({}, {}) to({}, {})", from.X, from.Y, to.X, to.Y);
        CURA_ERROR("  - 这可能导致后续处理问题");
    }

    start_source_point = source_segment.to();
    end_source_point = source_segment.from();
}

SkeletalTrapezoidation::SkeletalTrapezoidation(
    const Shape& polys,
    const BeadingStrategy& beading_strategy,
    AngleRadians transitioning_angle,
    coord_t discretization_step_size,
    coord_t transition_filter_dist,
    coord_t allowed_filter_deviation,
    coord_t beading_propagation_transition_dist,
    int layer_idx,
    SectionType section_type)
    : transitioning_angle_(transitioning_angle)
    , discretization_step_size_(discretization_step_size)
    , transition_filter_dist_(transition_filter_dist)
    , allowed_filter_deviation_(allowed_filter_deviation)
    , beading_propagation_transition_dist_(beading_propagation_transition_dist)
    , layer_idx_(layer_idx)
    , section_type_(section_type)
    , beading_strategy_(beading_strategy)
{
    // 初始化崩溃安全调试系统
    cura::CrashSafeDebug::initialize();

    scripta::log("skeletal_trapezoidation_0", polys, section_type, layer_idx);
    constructFromPolygons(polys);

    // 在构造完成后立即验证和修复图完整性
    validateAndFixGraphIntegrity();
}

void SkeletalTrapezoidation::constructFromPolygons(const Shape& polys)
{
    vd_edge_to_he_edge_.clear();
    vd_node_to_he_node_.clear();

    std::vector<Point2LL> points; // Remains empty

    std::vector<Segment> segments;
    for (size_t poly_idx = 0; poly_idx < polys.size(); poly_idx++)
    {
        const Polygon& poly = polys[poly_idx];
        for (size_t point_idx = 0; point_idx < poly.size(); point_idx++)
        {
            segments.emplace_back(&polys, poly_idx, point_idx);
        }
    }

    vd_t vonoroi_diagram;
    construct_voronoi(segments.begin(), segments.end(), &vonoroi_diagram);

    for (vd_t::cell_type cell : vonoroi_diagram.cells())
    {
        if (! cell.incident_edge())
        { // There is no spoon
            continue;
        }
        Point2LL start_source_point;
        Point2LL end_source_point;
        vd_t::edge_type* starting_vonoroi_edge = nullptr;
        vd_t::edge_type* ending_vonoroi_edge = nullptr;
        // Compute and store result in above variables

        if (cell.contains_point())
        {
            const bool keep_going = computePointCellRange(cell, start_source_point, end_source_point, starting_vonoroi_edge, ending_vonoroi_edge, points, segments);
            if (! keep_going)
            {
                continue;
            }
        }
        else
        {
            // 预检查：确保cell有有效的segment
            if (!cell.contains_segment())
            {
                spdlog::warn("跳过无效的segment cell");
                continue;
            }

            // 预检查：确保有足够的有限边
            int finite_edge_count = 0;
            vd_t::edge_type* edge = cell.incident_edge();
            do
            {
                if (!edge->is_infinite())
                {
                    finite_edge_count++;
                }
            } while (edge = edge->next(), edge != cell.incident_edge());

            if (finite_edge_count < 2)
            {
                spdlog::warn("跳过边数不足的segment cell (finite_edges: {})", finite_edge_count);
                continue;
            }

            try
            {
                computeSegmentCellRange(cell, start_source_point, end_source_point, starting_vonoroi_edge, ending_vonoroi_edge, points, segments);
            }
            catch (const std::exception& e)
            {
                spdlog::warn("跳过有问题的segment cell: {}", e.what());
                continue;
            }
        }

        if (! starting_vonoroi_edge || ! ending_vonoroi_edge)
        {
            // assert(false && "Each cell should start / end in a polygon vertex"); // 原始断言，注释保留以备回滚
            CURA_ERROR("BeadingStrategy错误: cell应该在多边形顶点开始/结束");
            CURA_ERROR("  - starting_vonoroi_edge: {}", static_cast<const void*>(starting_vonoroi_edge));
            CURA_ERROR("  - ending_vonoroi_edge: {}", static_cast<const void*>(ending_vonoroi_edge));
            CURA_ERROR("  - cell类型: {}", cell.contains_point() ? "point" : "segment");
            CURA_ERROR("  - 可能原因: Voronoi图构建不完整或cell范围计算失败");
            continue;
        }

        // Copy start to end edge to graph
        edge_t* prev_edge = nullptr;
        transferEdge(
            start_source_point,
            VoronoiUtils::p(starting_vonoroi_edge->vertex1()),
            *starting_vonoroi_edge,
            prev_edge,
            start_source_point,
            end_source_point,
            points,
            segments);
        node_t* starting_node = vd_node_to_he_node_[starting_vonoroi_edge->vertex0()];
        starting_node->data_.distance_to_boundary_ = 0;

        graph_.makeRib(prev_edge, start_source_point, end_source_point);
        for (vd_t::edge_type* vd_edge = starting_vonoroi_edge->next(); vd_edge != ending_vonoroi_edge; vd_edge = vd_edge->next())
        {
            // assert(vd_edge->is_finite()); // 原始断言，注释保留以备回滚
            if (!vd_edge->is_finite())
            {
                CURA_ERROR("BeadingStrategy错误: cell遍历中遇到无限边");
                CURA_ERROR("  - vd_edge地址: {}", static_cast<const void*>(vd_edge));
                CURA_ERROR("  - cell类型: {}", cell.contains_point() ? "point" : "segment");
                CURA_ERROR("  - 可能原因: Voronoi图包含无限边，需要跳过处理");
                continue; // 跳过无限边，继续处理下一条边
            }
            Point2LL v1 = VoronoiUtils::p(vd_edge->vertex0());
            Point2LL v2 = VoronoiUtils::p(vd_edge->vertex1());
            transferEdge(v1, v2, *vd_edge, prev_edge, start_source_point, end_source_point, points, segments);

            graph_.makeRib(prev_edge, start_source_point, end_source_point);
        }

        transferEdge(VoronoiUtils::p(ending_vonoroi_edge->vertex0()), end_source_point, *ending_vonoroi_edge, prev_edge, start_source_point, end_source_point, points, segments);
        prev_edge->to_->data_.distance_to_boundary_ = 0;
    }

    separatePointyQuadEndNodes();

    graph_.collapseSmallEdges();

    // Set [incident_edge] the the first possible edge that way we can iterate over all reachable edges from node.incident_edge,
    // without needing to iterate backward
    for (edge_t& edge : graph_.edges)
    {
        if (! edge.prev_)
        {
            edge.from_->incident_edge_ = &edge;
        }
    }

    // 在图构建完成后立即验证和修复图完整性
    validateAndFixGraphIntegrity();
}

void SkeletalTrapezoidation::separatePointyQuadEndNodes()
{
    std::unordered_set<node_t*> visited_nodes;
    for (edge_t& edge : graph_.edges)
    {
        if (edge.prev_)
        {
            continue;
        }
        edge_t* quad_start = &edge;
        if (visited_nodes.find(quad_start->from_) == visited_nodes.end())
        {
            visited_nodes.emplace(quad_start->from_);
        }
        else
        { // Needs to be duplicated
            graph_.nodes.emplace_back(*quad_start->from_);
            node_t* new_node = &graph_.nodes.back();
            new_node->incident_edge_ = quad_start;
            quad_start->from_ = new_node;
            quad_start->twin_->to_ = new_node;
        }
    }
}

//
// ^^^^^^^^^^^^^^^^^^^^^
//    INITIALIZATION
// =====================
//
// =====================
//    TRANSTISIONING
// vvvvvvvvvvvvvvvvvvvvv
//

void SkeletalTrapezoidation::generateToolpaths(std::vector<VariableWidthLines>& generated_toolpaths, bool filter_outermost_central_edges)
{
    p_generated_toolpaths = &generated_toolpaths;

    updateIsCentral();

    filterCentral(central_filter_dist_);

    if (filter_outermost_central_edges)
    {
        filterOuterCentral();
    }

    updateBeadCount();
    scripta::log(
        "st_graph_0",
        graph_,
        section_type_,
        layer_idx_,
        scripta::CellVDI{ "is_central",
                          [](const auto& edge)
                          {
                              return static_cast<int>(edge.data_.is_central);
                          } },
        scripta::CellVDI{ "type",
                          [](const auto& edge)
                          {
                              return static_cast<int>(edge.data_.type_);
                          } },
        scripta::PointVDI{ "distance_to_boundary",
                           [](const auto& node)
                           {
                               return node->data_.distance_to_boundary_;
                           } },
        scripta::PointVDI{ "bead_count",
                           [](const auto& node)
                           {
                               return node->data_.bead_count_;
                           } },
        scripta::PointVDI{ "transition_ratio",
                           [](const auto& node)
                           {
                               return node->data_.transition_ratio_;
                           } });

    filterNoncentralRegions();
    scripta::log(
        "st_graph_1",
        graph_,
        section_type_,
        layer_idx_,
        scripta::CellVDI{ "is_central",
                          [](const auto& edge)
                          {
                              return static_cast<int>(edge.data_.is_central);
                          } },
        scripta::CellVDI{ "type",
                          [](const auto& edge)
                          {
                              return static_cast<int>(edge.data_.type_);
                          } },
        scripta::PointVDI{ "distance_to_boundary",
                           [](const auto& node)
                           {
                               return node->data_.distance_to_boundary_;
                           } },
        scripta::PointVDI{ "bead_count",
                           [](const auto& node)
                           {
                               return node->data_.bead_count_;
                           } },
        scripta::PointVDI{ "transition_ratio",
                           [](const auto& node)
                           {
                               return node->data_.transition_ratio_;
                           } });

    generateTransitioningRibs();
    scripta::log(
        "st_graph_2",
        graph_,
        section_type_,
        layer_idx_,
        scripta::CellVDI{ "is_central",
                          [](const auto& edge)
                          {
                              return static_cast<int>(edge.data_.is_central);
                          } },
        scripta::CellVDI{ "type",
                          [](const auto& edge)
                          {
                              return static_cast<int>(edge.data_.type_);
                          } },
        scripta::PointVDI{ "distance_to_boundary",
                           [](const auto& node)
                           {
                               return node->data_.distance_to_boundary_;
                           } },
        scripta::PointVDI{ "bead_count",
                           [](const auto& node)
                           {
                               return node->data_.bead_count_;
                           } },
        scripta::PointVDI{ "transition_ratio",
                           [](const auto& node)
                           {
                               return node->data_.transition_ratio_;
                           } });

    generateExtraRibs();
    scripta::log(
        "st_graph_3",
        graph_,
        section_type_,
        layer_idx_,
        scripta::CellVDI{ "is_central",
                          [](const auto& edge)
                          {
                              return static_cast<int>(edge.data_.is_central);
                          } },
        scripta::CellVDI{ "type",
                          [](const auto& edge)
                          {
                              return static_cast<int>(edge.data_.type_);
                          } },
        scripta::PointVDI{ "distance_to_boundary",
                           [](const auto& node)
                           {
                               return node->data_.distance_to_boundary_;
                           } },
        scripta::PointVDI{ "bead_count",
                           [](const auto& node)
                           {
                               return node->data_.bead_count_;
                           } },
        scripta::PointVDI{ "transition_ratio",
                           [](const auto& node)
                           {
                               return node->data_.transition_ratio_;
                           } });

    generateSegments();
    scripta::log(
        "st_graph_4",
        graph_,
        section_type_,
        layer_idx_,
        scripta::CellVDI{ "is_central",
                          [](const auto& edge)
                          {
                              return static_cast<int>(edge.data_.is_central);
                          } },
        scripta::CellVDI{ "type",
                          [](const auto& edge)
                          {
                              return static_cast<int>(edge.data_.type_);
                          } },
        scripta::PointVDI{ "distance_to_boundary",
                           [](const auto& node)
                           {
                               return node->data_.distance_to_boundary_;
                           } },
        scripta::PointVDI{ "bead_count",
                           [](const auto& node)
                           {
                               return node->data_.bead_count_;
                           } },
        scripta::PointVDI{ "transition_ratio",
                           [](const auto& node)
                           {
                               return node->data_.transition_ratio_;
                           } });
}

void SkeletalTrapezoidation::updateIsCentral()
{
    //                                            _.-'^`      A and B are the endpoints of an edge we're checking.
    //                                      _.-'^`            Part of the line AB will be used as a cap,
    //                                _.-'^` \                because the polygon is too narrow there.
    //                          _.-'^`        \               If |AB| minus the cap is still bigger than dR,
    //                    _.-'^`               \ R2           the edge AB is considered central. It's then
    //              _.-'^` \              _.-'\`\             significant compared to the edges around it.
    //        _.-'^`        \R1     _.-'^`     '`\ dR
    //  _.-'^`a/2            \_.-'^`a             \           Line AR2 is parallel to the polygon contour.
    //  `^'-._````````````````A```````````v````````B```````   dR is the remaining diameter at B.
    //        `^'-._                     dD = |AB|            As a result, AB is less often central if the polygon
    //              `^'-._                                    corner is obtuse.
    //                             sin a = dR / dD

    spdlog::debug("=== 开始updateIsCentral处理 ===");
    spdlog::debug("图中总边数: {}", graph_.edges.size());

    coord_t outer_edge_filter_length = beading_strategy_.getTransitionThickness(0) / 2;
    spdlog::debug("outer_edge_filter_length: {}", outer_edge_filter_length);

    double cap = sin(beading_strategy_.getTransitioningAngle() * 0.5); // = cos(bisector_angle / 2)
    spdlog::debug("cap值: {}", cap);
    for (edge_t& edge : graph_.edges)
    {
        // 检查twin指针的有效性，添加更详细的调试信息
        if (! edge.twin_)
        {
            spdlog::warn("跳过没有twin的Voronoi边: from({}, {}) to({}, {})",
                        edge.from_->p_.X, edge.from_->p_.Y,
                        edge.to_->p_.X, edge.to_->p_.Y);
            // 为没有twin的边设置默认的central状态
            edge.data_.setIsCentral(false);
            continue;
        }
        if (edge.twin_->data_.centralIsSet())
        {
            edge.data_.setIsCentral(edge.twin_->data_.isCentral());
        }
        else if (edge.data_.type_ == SkeletalTrapezoidationEdge::EdgeType::EXTRA_VD)
        {
            edge.data_.setIsCentral(false);
        }
        else if (std::max(edge.from_->data_.distance_to_boundary_, edge.to_->data_.distance_to_boundary_) < outer_edge_filter_length)
        {
            edge.data_.setIsCentral(false);
        }
        else
        {
            Point2LL a = edge.from_->p_;
            Point2LL b = edge.to_->p_;
            Point2LL ab = b - a;
            coord_t dR = std::abs(edge.to_->data_.distance_to_boundary_ - edge.from_->data_.distance_to_boundary_);
            coord_t dD = vSize(ab);
            edge.data_.setIsCentral(dR < dD * cap);
        }
    }
    spdlog::debug("=== updateIsCentral处理完成 ===");
}

void SkeletalTrapezoidation::filterCentral(coord_t max_length)
{
    for (edge_t& edge : graph_.edges)
    {
        if (isEndOfCentral(edge) && edge.to_->isLocalMaximum() && ! edge.to_->isLocalMaximum())
        {
            filterCentral(edge.twin_, 0, max_length);
        }
    }
}

bool SkeletalTrapezoidation::filterCentral(edge_t* starting_edge, coord_t traveled_dist, coord_t max_length)
{
    coord_t length = vSize(starting_edge->from_->p_ - starting_edge->to_->p_);
    if (traveled_dist + length > max_length)
    {
        return false;
    }

    bool should_dissolve = true; // Should we unmark this as central and propagate that?
    for (edge_t* next_edge = starting_edge->next_; next_edge && next_edge != starting_edge->twin_; next_edge = next_edge->twin_->next_)
    {
        if (next_edge->data_.isCentral())
        {
            should_dissolve &= filterCentral(next_edge, traveled_dist + length, max_length);
        }
    }

    should_dissolve &= ! starting_edge->to_->isLocalMaximum(); // Don't filter central regions with a local maximum!
    if (should_dissolve)
    {
        starting_edge->data_.setIsCentral(false);
        starting_edge->twin_->data_.setIsCentral(false);
    }
    return should_dissolve;
}

void SkeletalTrapezoidation::filterOuterCentral()
{
    for (edge_t& edge : graph_.edges)
    {
        if (! edge.prev_)
        {
            edge.data_.setIsCentral(false);
            edge.twin_->data_.setIsCentral(false);
        }
    }
}

void SkeletalTrapezoidation::updateBeadCount()
{
    for (edge_t& edge : graph_.edges)
    {
        if (edge.data_.isCentral())
        {
            edge.to_->data_.bead_count_ = beading_strategy_.getOptimalBeadCount(edge.to_->data_.distance_to_boundary_ * 2);
        }
    }

    // Fix bead count at locally maximal R, also for central regions!! See TODO s in generateTransitionEnd(.)
    for (node_t& node : graph_.nodes)
    {
        if (node.isLocalMaximum())
        {
            if (node.data_.distance_to_boundary_ < 0)
            {
                spdlog::warn("Distance to boundary not yet computed for local maximum!");
                node.data_.distance_to_boundary_ = std::numeric_limits<coord_t>::max();
                edge_t* edge = node.incident_edge_;
                do
                {
                    node.data_.distance_to_boundary_ = std::min(node.data_.distance_to_boundary_, edge->to_->data_.distance_to_boundary_ + vSize(edge->from_->p_ - edge->to_->p_));
                } while (edge = edge->twin_->next_, edge != node.incident_edge_);
            }
            coord_t bead_count = beading_strategy_.getOptimalBeadCount(node.data_.distance_to_boundary_ * 2);
            node.data_.bead_count_ = bead_count;
        }
    }
}

void SkeletalTrapezoidation::filterNoncentralRegions()
{
    for (edge_t& edge : graph_.edges)
    {
        if (! isEndOfCentral(edge))
        {
            continue;
        }
        if (edge.to_->data_.bead_count_ < 0 && edge.to_->data_.distance_to_boundary_ != 0)
        {
            CURA_ERROR("BeadingStrategy警告: 在边界遇到未初始化的bead");
            CURA_ERROR("  - bead_count: {}", edge.to_->data_.bead_count_);
            CURA_ERROR("  - distance_to_boundary: {}", edge.to_->data_.distance_to_boundary_);
        }
        // assert(edge.to_->data_.bead_count_ >= 0 || edge.to_->data_.distance_to_boundary_ == 0); // 原始断言，注释保留以备回滚
        if (!(edge.to_->data_.bead_count_ >= 0 || edge.to_->data_.distance_to_boundary_ == 0))
        {
            CURA_ERROR("BeadingStrategy错误: bead计数验证失败");
            CURA_ERROR("  - edge.to_位置: ({}, {})", edge.to_->p_.X, edge.to_->p_.Y);
            CURA_ERROR("  - bead_count: {}", edge.to_->data_.bead_count_);
            CURA_ERROR("  - distance_to_boundary: {}", edge.to_->data_.distance_to_boundary_);
            CURA_ERROR("  - 规则: bead_count应该>=0，或者distance_to_boundary应该==0");
            CURA_ERROR("  - 可能原因: bead计数初始化失败或边界距离计算错误");
            // 尝试修复：如果在边界上，设置bead_count为0
            if (edge.to_->data_.distance_to_boundary_ == 0)
            {
                edge.to_->data_.bead_count_ = 0;
                CURA_ERROR("  - 已修复: 将边界节点的bead_count设置为0");
            }
        }
        constexpr coord_t max_dist = 400;
        filterNoncentralRegions(&edge, edge.to_->data_.bead_count_, 0, max_dist);
    }
}

bool SkeletalTrapezoidation::filterNoncentralRegions(edge_t* to_edge, coord_t bead_count, coord_t traveled_dist, coord_t max_dist)
{
    coord_t r = to_edge->to_->data_.distance_to_boundary_;

    edge_t* next_edge = to_edge->next_;
    for (; next_edge && next_edge != to_edge->twin_; next_edge = next_edge->twin_->next_)
    {
        if (next_edge->to_->data_.distance_to_boundary_ >= r || shorterThen(next_edge->to_->p_ - next_edge->from_->p_, 10))
        {
            break; // Only walk upward
        }
    }
    if (next_edge == to_edge->twin_ || ! next_edge)
    {
        return false;
    }

    const coord_t length = vSize(next_edge->to_->p_ - next_edge->from_->p_);

    bool dissolve = false;
    if (next_edge->to_->data_.bead_count_ == bead_count)
    {
        dissolve = true;
    }
    else if (next_edge->to_->data_.bead_count_ < 0)
    {
        dissolve = filterNoncentralRegions(next_edge, bead_count, traveled_dist + length, max_dist);
    }
    else // Upward bead count is different
    {
        // Dissolve if two central regions with different bead count are closer together than the max_dist (= transition distance)
        dissolve = (traveled_dist + length < max_dist) && std::abs(next_edge->to_->data_.bead_count_ - bead_count) == 1;
    }

    if (dissolve)
    {
        next_edge->data_.setIsCentral(true);
        next_edge->twin_->data_.setIsCentral(true);
        next_edge->to_->data_.bead_count_ = beading_strategy_.getOptimalBeadCount(next_edge->to_->data_.distance_to_boundary_ * 2);
        next_edge->to_->data_.transition_ratio_ = 0;
    }
    return dissolve; // Dissolving only depend on the one edge going upward. There cannot be multiple edges going upward.
}

void SkeletalTrapezoidation::generateTransitioningRibs()
{
    CURA_ERROR_FLUSH("=== 开始生成过渡肋骨 ===");
    CURA_ERROR_FLUSH_F("图中边缘总数: %zu", graph_.edges.size());

    // 在生成过渡前验证和修复图完整性
    validateAndFixGraphIntegrity();

    // Store the upward edges to the transitions.
    // We only store the halfedge for which the distance_to_boundary is higher at the end than at the beginning.
    ptr_vector_t<std::list<TransitionMiddle>> edge_transitions;

    CURA_ERROR_FLUSH("开始生成过渡中点...");
    generateTransitionMids(edge_transitions);
    CURA_ERROR_FLUSH_F("过渡中点生成完成，edge_transitions大小: %zu", edge_transitions.size());

    CURA_ERROR_FLUSH("开始验证边缘过渡一致性...");
    size_t central_edges_count = 0;
    size_t different_bead_count_edges = 0;
    size_t missing_transitions_count = 0;

    for (edge_t& edge : graph_.edges)
    { // Check if there is a transition in between nodes with different bead counts
        if (edge.data_.isCentral())
        {
            central_edges_count++;
            if (edge.from_->data_.bead_count_ != edge.to_->data_.bead_count_)
            {
                different_bead_count_edges++;
                // 使用崩溃安全检查
                if (!CURA_CHECK_WITH_ERROR(edge.data_.hasTransitions() || (edge.twin_ && edge.twin_->data_.hasTransitions()),
                    "中心边缘的bead计数不同但没有过渡"))
                {
                    missing_transitions_count++;
                    CURA_ERROR_FLUSH_F("过渡缺失详情 #%zu:", missing_transitions_count);
                    CURA_ERROR_FLUSH_F("  - edge地址: %p", static_cast<void*>(&edge));
                    CURA_ERROR_FLUSH_F("  - twin地址: %p", static_cast<void*>(edge.twin_));
                    CURA_ERROR_FLUSH_F("  - from_bead_count: %d, to_bead_count: %d",
                                      edge.from_->data_.bead_count_, edge.to_->data_.bead_count_);
                    CURA_ERROR_FLUSH_F("  - from_distance: %d, to_distance: %d",
                                      edge.from_->data_.distance_to_boundary_, edge.to_->data_.distance_to_boundary_);
                    CURA_ERROR_FLUSH_F("  - edge.hasTransitions: %s, twin.hasTransitions: %s",
                                      edge.data_.hasTransitions() ? "true" : "false",
                                      (edge.twin_ && edge.twin_->data_.hasTransitions()) ? "true" : "false");
                    CURA_ERROR_FLUSH_F("  - edge位置: from(%d,%d) to(%d,%d)",
                                      edge.from_->p_.X, edge.from_->p_.Y, edge.to_->p_.X, edge.to_->p_.Y);

                    // 如果错误太多，触发崩溃安全断言
                    if (missing_transitions_count > 10)
                    {
                        CURA_ASSERT_WITH_INFO(false, "过渡缺失错误过多，程序将崩溃以便调试");
                    }
                }
            }
        }
    }

    CURA_ERROR_FLUSH_F("边缘验证完成: 总边缘=%zu, 中心边缘=%zu, 不同bead计数=%zu, 缺失过渡=%zu",
                      graph_.edges.size(), central_edges_count, different_bead_count_edges, missing_transitions_count);

    // 如果没有需要过渡的边缘，提前返回避免后续处理空数据
    if (different_bead_count_edges == 0)
    {
        CURA_ERROR_FLUSH("没有需要过渡的边缘，跳过过渡处理");

        // 在过渡处理后验证和修复图完整性
        validateAndFixGraphIntegrity();

        return;
    }

    filterTransitionMids();

    ptr_vector_t<std::list<TransitionEnd>> edge_transition_ends; // We only map the half edge in the upward direction. mapped items are not sorted
    generateAllTransitionEnds(edge_transition_ends);

    applyTransitions(edge_transition_ends);
    // Note that the shared pointer lists will be out of scope and thus destroyed here, since the remaining refs are weak_ptr.
}


void SkeletalTrapezoidation::generateTransitionMids(ptr_vector_t<std::list<TransitionMiddle>>& edge_transitions)
{
    CURA_ERROR_FLUSH("=== 开始生成过渡中点 ===");

    // 在生成过渡中点前验证和修复图完整性
    validateAndFixGraphIntegrity();
    size_t total_edges = graph_.edges.size();
    size_t central_unset_count = 0;
    size_t central_edges = 0;
    size_t processed_edges = 0;

    for (edge_t& edge : graph_.edges)
    {
        processed_edges++;

        // 使用崩溃安全检查
        if (!CURA_CHECK_WITH_ERROR(edge.data_.centralIsSet(), "边缘的central状态未设置"))
        {
            central_unset_count++;
            CURA_ERROR_FLUSH_F("Central状态未设置 #%zu:", central_unset_count);
            CURA_ERROR_FLUSH_F("  - edge地址: %p", static_cast<void*>(&edge));
            CURA_ERROR_FLUSH_F("  - edge位置: from(%d,%d) to(%d,%d)",
                              edge.from_->p_.X, edge.from_->p_.Y, edge.to_->p_.X, edge.to_->p_.Y);
            CURA_ERROR_FLUSH_F("  - 进度: %zu/%zu", processed_edges, total_edges);

            // 如果太多边缘未设置central状态，触发崩溃
            if (central_unset_count > 100)
            {
                CURA_ASSERT_WITH_INFO(false, "过多边缘的central状态未设置，可能updateIsCentral()函数未正确执行");
            }
            continue;
        }

        if (! edge.data_.isCentral())
        { // Only central regions introduce transitions
            continue;
        }

        central_edges++;
        coord_t start_R = edge.from_->data_.distance_to_boundary_;
        coord_t end_R = edge.to_->data_.distance_to_boundary_;
        int start_bead_count = edge.from_->data_.bead_count_;
        int end_bead_count = edge.to_->data_.bead_count_;

        // 进一步减少调试输出频率，避免多线程混乱
        if (central_edges <= 2 || central_edges % 10000 == 0)  // 只记录前2个和每10000个
        {
            CURA_ERROR_FLUSH_F("处理中心边缘 #%zu:", central_edges);
            CURA_ERROR_FLUSH_F("  - start_R=%d, end_R=%d, start_bead=%d, end_bead=%d",
                              start_R, end_R, start_bead_count, end_bead_count);
            CURA_ERROR_FLUSH_F("  - 位置: from(%d,%d) to(%d,%d)",
                              edge.from_->p_.X, edge.from_->p_.Y, edge.to_->p_.X, edge.to_->p_.Y);
        }

        if (start_R == end_R)
        { // No transitions occur when both end points have the same distance_to_boundary
            // 使用崩溃安全检查
            if (!CURA_CHECK_WITH_ERROR(edge.from_->data_.bead_count_ == edge.to_->data_.bead_count_,
                "相同边界距离但bead计数不同"))
            {
                CURA_ERROR_FLUSH_F("距离相等但bead计数不同的边缘:");
                CURA_ERROR_FLUSH_F("  - start_R = end_R = %d", start_R);
                CURA_ERROR_FLUSH_F("  - start_bead_count: %d, end_bead_count: %d",
                                  edge.from_->data_.bead_count_, edge.to_->data_.bead_count_);
                CURA_ERROR_FLUSH_F("  - edge位置: from(%d,%d) to(%d,%d)",
                                  edge.from_->p_.X, edge.from_->p_.Y, edge.to_->p_.X, edge.to_->p_.Y);
                CURA_ERROR_FLUSH_F("  - 可能原因: bead计数计算不一致或边界距离计算精度问题");

                // 尝试获取更多上下文信息
                coord_t optimal_start = beading_strategy_.getOptimalBeadCount(start_R * 2);
                coord_t optimal_end = beading_strategy_.getOptimalBeadCount(end_R * 2);
                CURA_ERROR_FLUSH_F("  - 理论最优bead计数: start=%d, end=%d", optimal_start, optimal_end);
            }
            continue;
        }
        else if (start_R > end_R)
        { // Only consider those half-edges which are going from a lower to a higher distance_to_boundary
            continue;
        }

        if (edge.from_->data_.bead_count_ == edge.to_->data_.bead_count_)
        { // No transitions should occur according to the enforced bead counts
            continue;
        }

        if (start_bead_count > beading_strategy_.getOptimalBeadCount(start_R * 2) || end_bead_count > beading_strategy_.getOptimalBeadCount(end_R * 2))
        { // Wasn't the case earlier in this function because of already introduced transitions
            CURA_ERROR("BeadingStrategy错误: 过渡段重叠");
            CURA_ERROR("  - start_bead_count: {}, optimal: {}", start_bead_count, beading_strategy_.getOptimalBeadCount(start_R * 2));
            CURA_ERROR("  - end_bead_count: {}, optimal: {}", end_bead_count, beading_strategy_.getOptimalBeadCount(end_R * 2));
            CURA_ERROR("  - start_R: {}, end_R: {}", start_R, end_R);
        }

        // 使用崩溃安全检查过渡方向
        if (!CURA_CHECK_WITH_ERROR(start_R < end_R, "过渡方向错误，期望start_R < end_R"))
        {
            CURA_ERROR_FLUSH_F("过渡方向错误的边缘 #%zu:", central_edges);
            CURA_ERROR_FLUSH_F("  - start_R: %d, end_R: %d (差值: %d)", start_R, end_R, end_R - start_R);
            CURA_ERROR_FLUSH_F("  - start_bead: %d, end_bead: %d", start_bead_count, end_bead_count);
            CURA_ERROR_FLUSH_F("  - edge位置: from(%d,%d) to(%d,%d)",
                              edge.from_->p_.X, edge.from_->p_.Y, edge.to_->p_.X, edge.to_->p_.Y);
            CURA_ERROR_FLUSH_F("  - edge长度: %d", vSize(edge.from_->p_ - edge.to_->p_));

            // 检查twin边缘的情况
            if (edge.twin_)
            {
                CURA_ERROR_FLUSH_F("  - twin边缘: from_R=%d, to_R=%d",
                                  edge.twin_->from_->data_.distance_to_boundary_,
                                  edge.twin_->to_->data_.distance_to_boundary_);
            }

            CURA_ERROR_FLUSH("  - 期望: start_R < end_R (从小R过渡到大R)");
            CURA_ERROR_FLUSH("  - 可能原因: 边缘方向错误或距离计算问题");
            CURA_ERROR_FLUSH("  - 修复: 跳过此边缘，继续处理");
            continue; // 跳过这个错误的过渡
        }
        coord_t edge_size = vSize(edge.from_->p_ - edge.to_->p_);

        // 验证边缘长度
        if (!CURA_CHECK_WITH_ERROR(edge_size > 0, "边缘长度为零或负数"))
        {
            CURA_ERROR_FLUSH_F("无效边缘长度: %d", edge_size);
            CURA_ERROR_FLUSH_F("  - 位置: from(%d,%d) to(%d,%d)",
                              edge.from_->p_.X, edge.from_->p_.Y, edge.to_->p_.X, edge.to_->p_.Y);
            continue;
        }

        CURA_ERROR_FLUSH_F("处理过渡循环: start_bead=%d, end_bead=%d, edge_size=%d",
                          start_bead_count, end_bead_count, edge_size);

        for (int transition_lower_bead_count = start_bead_count; transition_lower_bead_count < end_bead_count; transition_lower_bead_count++)
        {
            coord_t mid_R = beading_strategy_.getTransitionThickness(transition_lower_bead_count) / 2;
            coord_t original_mid_R = mid_R;

            if (mid_R > end_R)
            {
                CURA_ERROR_FLUSH_F("过渡在段外(上界): mid_R=%d > end_R=%d, 调整为end_R", mid_R, end_R);
                mid_R = end_R;
            }
            if (mid_R < start_R)
            {
                CURA_ERROR_FLUSH_F("过渡在段外(下界): mid_R=%d < start_R=%d, 调整为start_R", mid_R, start_R);
                mid_R = start_R;
            }

            // 验证除法安全性
            coord_t denominator = end_R - start_R;
            if (!CURA_CHECK_WITH_ERROR(denominator != 0, "过渡计算中分母为零"))
            {
                CURA_ERROR_FLUSH_F("除零错误: end_R=%d, start_R=%d", end_R, start_R);
                continue;
            }

            coord_t mid_pos = edge_size * (mid_R - start_R) / denominator;

            CURA_ERROR_FLUSH_F("过渡计算: bead_count=%d, original_mid_R=%d, adjusted_mid_R=%d, mid_pos=%d",
                              transition_lower_bead_count, original_mid_R, mid_R, mid_pos);
            // 使用崩溃安全检查过渡位置范围
            if (!CURA_CHECK_WITH_ERROR(mid_pos >= 0 && mid_pos <= edge_size, "过渡中点超出边缘范围"))
            {
                CURA_ERROR_FLUSH_F("过渡位置超出范围:");
                CURA_ERROR_FLUSH_F("  - mid_pos: %d, edge_size: %d (超出: %s)",
                                  mid_pos, edge_size,
                                  mid_pos < 0 ? "下界" : "上界");
                CURA_ERROR_FLUSH_F("  - mid_R: %d, start_R: %d, end_R: %d", mid_R, start_R, end_R);
                CURA_ERROR_FLUSH_F("  - transition_lower_bead_count: %d", transition_lower_bead_count);
                CURA_ERROR_FLUSH_F("  - 计算: %d * (%d - %d) / (%d - %d) = %d",
                                  edge_size, mid_R, start_R, end_R, start_R, mid_pos);
                CURA_ERROR_FLUSH_F("  - 分子: %d, 分母: %d", edge_size * (mid_R - start_R), end_R - start_R);

                // 如果位置严重超出范围，可能是计算错误
                if (mid_pos < -edge_size || mid_pos > 2 * edge_size)
                {
                    CURA_ASSERT_WITH_INFO(false, "过渡位置严重超出范围，可能存在计算错误");
                }
                continue; // 跳过这个无效的过渡
            }
            auto transitions = edge.data_.getTransitions();
            constexpr bool ignore_empty = true;

            // 使用崩溃安全检查过渡顺序
            if (edge.data_.hasTransitions(ignore_empty))
            {
                coord_t last_pos = transitions->back().pos_;
                if (!CURA_CHECK_WITH_ERROR(mid_pos >= last_pos, "过渡中点在最后一个过渡之前"))
                {
                    CURA_ERROR_FLUSH_F("过渡顺序错误:");
                    CURA_ERROR_FLUSH_F("  - mid_pos: %d, last_transition_pos: %d (差值: %d)",
                                      mid_pos, last_pos, mid_pos - last_pos);
                    CURA_ERROR_FLUSH_F("  - mid_R: %d, last_transition_lower_bead_count: %d",
                                      mid_R, transitions->back().lower_bead_count_);
                    CURA_ERROR_FLUSH_F("  - current_transition_bead_count: %d", transition_lower_bead_count);
                    CURA_ERROR_FLUSH_F("  - transitions总数: %zu", transitions->size());
                    CURA_ERROR_FLUSH_F("  - edge位置: from(%d,%d) to(%d,%d)",
                                      edge.from_->p_.X, edge.from_->p_.Y, edge.to_->p_.X, edge.to_->p_.Y);

                    // 输出所有现有过渡的信息
                    size_t idx = 0;
                    for (const auto& trans : *transitions)
                    {
                        CURA_ERROR_FLUSH_F("    过渡[%zu]: pos=%d, lower_bead=%d",
                                          idx++, trans.pos_, trans.lower_bead_count_);
                    }

                    CURA_ERROR_FLUSH("  - 可能原因: 过渡顺序错误或位置计算问题");
                    continue; // 跳过这个无序的过渡
                }
            }
            if (! edge.data_.hasTransitions(ignore_empty))
            {
                edge_transitions.emplace_back(std::make_shared<std::list<TransitionMiddle>>());
                edge.data_.setTransitions(edge_transitions.back()); // initialization
                transitions = edge.data_.getTransitions();
            }
            transitions->emplace_back(mid_pos, transition_lower_bead_count, mid_R);
        }

        // 最终验证：边缘两端bead计数不同时必须有过渡
        if (!CURA_CHECK_WITH_ERROR(
            (edge.from_->data_.bead_count_ == edge.to_->data_.bead_count_) || edge.data_.hasTransitions(),
            "边缘两端bead计数不同但没有过渡"))
        {
            CURA_ERROR_FLUSH_F("最终验证失败:");
            CURA_ERROR_FLUSH_F("  - from_bead_count: %d, to_bead_count: %d",
                              edge.from_->data_.bead_count_, edge.to_->data_.bead_count_);
            CURA_ERROR_FLUSH_F("  - hasTransitions: %s", edge.data_.hasTransitions() ? "true" : "false");
            CURA_ERROR_FLUSH_F("  - edge位置: from(%d,%d) to(%d,%d)",
                              edge.from_->p_.X, edge.from_->p_.Y, edge.to_->p_.X, edge.to_->p_.Y);
            CURA_ERROR_FLUSH("  - 规则: 如果两端bead计数不同，必须有过渡");
            CURA_ERROR_FLUSH("  - 可能原因: 过渡生成失败或bead计数设置错误");
        }
    }

    // 输出统计信息
    CURA_ERROR_FLUSH_F("=== 过渡中点生成完成 ===");
    CURA_ERROR_FLUSH_F("总边缘: %zu, 中心边缘: %zu, Central未设置: %zu",
                      total_edges, central_edges, central_unset_count);
    CURA_ERROR_FLUSH_F("edge_transitions大小: %zu", edge_transitions.size());

    // 如果有太多问题，提前返回避免后续崩溃
    if (central_unset_count > total_edges / 10)  // 超过10%的边缘有问题
    {
        CURA_ERROR_FLUSH_F("警告: 过多边缘存在问题 (%zu/%zu)，可能影响后续处理",
                          central_unset_count, total_edges);
    }
}

void SkeletalTrapezoidation::filterTransitionMids()
{
    for (edge_t& edge : graph_.edges)
    {
        if (! edge.data_.hasTransitions())
        {
            continue;
        }
        auto& transitions = *edge.data_.getTransitions();

        // This is how stuff should be stored in transitions
        // assert(transitions.front().lower_bead_count_ <= transitions.back().lower_bead_count_); // 原始断言，注释保留以备回滚
        if (transitions.front().lower_bead_count_ > transitions.back().lower_bead_count_)
        {
            CURA_ERROR("BeadingStrategy错误: 过渡序列的bead计数顺序错误");
            CURA_ERROR("  - front_lower_bead_count: {}, back_lower_bead_count: {}",
                      transitions.front().lower_bead_count_, transitions.back().lower_bead_count_);
            CURA_ERROR("  - transitions.size(): {}", transitions.size());
            CURA_ERROR("  - 期望: front <= back (过渡应该按bead计数递增排序)");
            CURA_ERROR("  - 可能原因: 过渡排序错误或计算错误");
            continue; // 跳过这个有问题的边缘
        }

        // assert(edge.from_->data_.distance_to_boundary_ <= edge.to_->data_.distance_to_boundary_); // 原始断言，注释保留以备回滚
        if (edge.from_->data_.distance_to_boundary_ > edge.to_->data_.distance_to_boundary_)
        {
            CURA_ERROR("BeadingStrategy错误: 边缘起点到边界距离大于终点距离");
            CURA_ERROR("  - from_distance: {}, to_distance: {}",
                      edge.from_->data_.distance_to_boundary_, edge.to_->data_.distance_to_boundary_);
            CURA_ERROR("  - edge位置: from({}, {}) to({}, {})",
                      edge.from_->p_.X, edge.from_->p_.Y, edge.to_->p_.X, edge.to_->p_.Y);
            CURA_ERROR("  - 可能原因: 距离计算错误或边缘方向错误");
            continue; // 跳过这个有问题的边缘
        }

        const Point2LL a = edge.from_->p_;
        const Point2LL b = edge.to_->p_;
        Point2LL ab = b - a;
        coord_t ab_size = vSize(ab);

        bool going_up = true;
        std::list<TransitionMidRef> to_be_dissolved_back
            = dissolveNearbyTransitions(&edge, transitions.back(), ab_size - transitions.back().pos_, transition_filter_dist_, going_up);
        bool should_dissolve_back = ! to_be_dissolved_back.empty();
        for (TransitionMidRef& ref : to_be_dissolved_back)
        {
            dissolveBeadCountRegion(&edge, transitions.back().lower_bead_count_ + 1, transitions.back().lower_bead_count_);
            ref.edge_->data_.getTransitions()->erase(ref.transition_it_);
        }

        {
            coord_t trans_bead_count = transitions.back().lower_bead_count_;
            coord_t upper_transition_half_length = (1.0 - beading_strategy_.getTransitionAnchorPos(trans_bead_count)) * beading_strategy_.getTransitioningLength(trans_bead_count);
            should_dissolve_back |= filterEndOfCentralTransition(&edge, ab_size - transitions.back().pos_, upper_transition_half_length, trans_bead_count);
        }

        if (should_dissolve_back)
        {
            transitions.pop_back();
        }
        if (transitions.empty())
        { // FilterEndOfCentralTransition gives inconsistent new bead count when executing for the same transition in two directions.
            continue;
        }

        going_up = false;
        std::list<TransitionMidRef> to_be_dissolved_front = dissolveNearbyTransitions(edge.twin_, transitions.front(), transitions.front().pos_, transition_filter_dist_, going_up);
        bool should_dissolve_front = ! to_be_dissolved_front.empty();
        for (TransitionMidRef& ref : to_be_dissolved_front)
        {
            dissolveBeadCountRegion(edge.twin_, transitions.front().lower_bead_count_, transitions.front().lower_bead_count_ + 1);
            ref.edge_->data_.getTransitions()->erase(ref.transition_it_);
        }

        {
            coord_t trans_bead_count = transitions.front().lower_bead_count_;
            coord_t lower_transition_half_length = beading_strategy_.getTransitionAnchorPos(trans_bead_count) * beading_strategy_.getTransitioningLength(trans_bead_count);
            should_dissolve_front |= filterEndOfCentralTransition(edge.twin_, transitions.front().pos_, lower_transition_half_length, trans_bead_count + 1);
        }

        if (should_dissolve_front)
        {
            transitions.pop_front();
        }
        if (transitions.empty())
        { // FilterEndOfCentralTransition gives inconsistent new bead count when executing for the same transition in two directions.
            continue;
        }
    }
}

std::list<SkeletalTrapezoidation::TransitionMidRef>
    SkeletalTrapezoidation::dissolveNearbyTransitions(edge_t* edge_to_start, TransitionMiddle& origin_transition, coord_t traveled_dist, coord_t max_dist, bool going_up)
{
    std::list<TransitionMidRef> to_be_dissolved;
    if (traveled_dist > max_dist)
    {
        return to_be_dissolved;
    }
    bool should_dissolve = true;
    for (edge_t* edge = edge_to_start->next_; edge && edge != edge_to_start->twin_; edge = edge->twin_->next_)
    {
        if (! edge->data_.isCentral())
        {
            continue;
        }

        Point2LL a = edge->from_->p_;
        Point2LL b = edge->to_->p_;
        Point2LL ab = b - a;
        coord_t ab_size = vSize(ab);
        bool is_aligned = edge->isUpward();
        edge_t* aligned_edge = is_aligned ? edge : edge->twin_;
        bool seen_transition_on_this_edge = false;

        const coord_t origin_radius = origin_transition.feature_radius_;
        const coord_t radius_here = edge->from_->data_.distance_to_boundary_;
        const bool dissolve_result_is_odd = bool(origin_transition.lower_bead_count_ % 2) == going_up;
        const coord_t width_deviation = std::abs(origin_radius - radius_here) * 2; // times by two because the deviation happens at both sides of the significant edge
        const coord_t line_width_deviation = dissolve_result_is_odd
                                               ? width_deviation
                                               : width_deviation / 2; // assume the deviation will be split over either 1 or 2 lines, i.e. assume wall_distribution_count = 1
        if (line_width_deviation > allowed_filter_deviation_)
        {
            should_dissolve = false;
        }

        if (should_dissolve && aligned_edge->data_.hasTransitions())
        {
            auto& transitions = *aligned_edge->data_.getTransitions();
            for (auto transition_it = transitions.begin(); transition_it != transitions.end(); ++transition_it)
            { // Note: this is not necessarily iterating in the traveling direction!
                // Check whether we should dissolve
                coord_t pos = is_aligned ? transition_it->pos_ : ab_size - transition_it->pos_;
                if (traveled_dist + pos < max_dist && transition_it->lower_bead_count_ == origin_transition.lower_bead_count_) // Only dissolve local optima
                {
                    if (traveled_dist + pos < beading_strategy_.getTransitioningLength(transition_it->lower_bead_count_))
                    {
                        // Consecutive transitions both in/decreasing in bead count should never be closer together than the transition distance
                        // assert(going_up != is_aligned || transition_it->lower_bead_count_ == 0); // 原始断言，注释保留以备回滚
                        if (!(going_up != is_aligned || transition_it->lower_bead_count_ == 0))
                        {
                            CURA_ERROR("BeadingStrategy错误: 连续过渡距离太近");
                            CURA_ERROR("  - going_up: {}, is_aligned: {}", going_up, is_aligned);
                            CURA_ERROR("  - transition_lower_bead_count: {}", transition_it->lower_bead_count_);
                            CURA_ERROR("  - traveled_dist + pos: {}, transition_length: {}",
                                      traveled_dist + pos, beading_strategy_.getTransitioningLength(transition_it->lower_bead_count_));
                            CURA_ERROR("  - 规则: 连续的增减bead计数过渡不应该比过渡距离更近");
                            CURA_ERROR("  - 可能原因: 过渡距离计算错误或过渡位置重叠");
                        }
                    }
                    to_be_dissolved.emplace_back(aligned_edge, transition_it);
                    seen_transition_on_this_edge = true;
                }
            }
        }
        if (should_dissolve && ! seen_transition_on_this_edge)
        {
            std::list<SkeletalTrapezoidation::TransitionMidRef> to_be_dissolved_here
                = dissolveNearbyTransitions(edge, origin_transition, traveled_dist + ab_size, max_dist, going_up);
            if (to_be_dissolved_here.empty())
            { // The region is too long to be dissolved in this direction, so it cannot be dissolved in any direction.
                to_be_dissolved.clear();
                return to_be_dissolved;
            }
            to_be_dissolved.splice(to_be_dissolved.end(), to_be_dissolved_here); // Transfer to_be_dissolved_here into to_be_dissolved
            should_dissolve = should_dissolve && ! to_be_dissolved.empty();
        }
    }

    if (! should_dissolve)
    {
        to_be_dissolved.clear();
    }

    return to_be_dissolved;
}


void SkeletalTrapezoidation::dissolveBeadCountRegion(edge_t* edge_to_start, coord_t from_bead_count, coord_t to_bead_count)
{
    CURA_ERROR_FLUSH_F("=== 溶解Bead计数区域 ===");
    CURA_ERROR_FLUSH_F("参数: from_bead_count=%d, to_bead_count=%d", from_bead_count, to_bead_count);

    // 使用崩溃安全检查参数有效性
    if (!CURA_CHECK_WITH_ERROR(from_bead_count != to_bead_count, "尝试溶解相同bead计数的区域"))
    {
        CURA_ERROR_FLUSH_F("参数错误:");
        CURA_ERROR_FLUSH_F("  - from_bead_count: %d, to_bead_count: %d", from_bead_count, to_bead_count);
        if (edge_to_start) {
            CURA_ERROR_FLUSH_F("  - edge_to_start位置: from(%d,%d) to(%d,%d)",
                              edge_to_start->from_->p_.X, edge_to_start->from_->p_.Y,
                              edge_to_start->to_->p_.X, edge_to_start->to_->p_.Y);
        } else {
            CURA_ERROR_FLUSH("  - edge_to_start: nullptr");
        }
        CURA_ERROR_FLUSH("  - 可能原因: 函数调用参数错误或bead计数计算错误");
        return;
    }

    // 验证edge_to_start有效性
    if (!CURA_CHECK_WITH_ERROR(edge_to_start != nullptr, "edge_to_start为空指针"))
    {
        CURA_ERROR_FLUSH("edge_to_start为空，无法进行溶解操作");
        return;
    }

    if (edge_to_start->to_->data_.bead_count_ != from_bead_count)
    {
        return;
    }

    edge_to_start->to_->data_.bead_count_ = to_bead_count;
    for (edge_t* edge = edge_to_start->next_; edge && edge != edge_to_start->twin_; edge = edge->twin_->next_)
    {
        if (! edge->data_.isCentral())
        {
            continue;
        }
        dissolveBeadCountRegion(edge, from_bead_count, to_bead_count);
    }
}

bool SkeletalTrapezoidation::filterEndOfCentralTransition(edge_t* edge_to_start, coord_t traveled_dist, coord_t max_dist, coord_t replacing_bead_count)
{
    if (traveled_dist > max_dist)
    {
        return false;
    }

    bool is_end_of_central = true;
    bool should_dissolve = false;
    for (edge_t* next_edge = edge_to_start->next_; next_edge && next_edge != edge_to_start->twin_; next_edge = next_edge->twin_->next_)
    {
        if (next_edge->data_.isCentral())
        {
            coord_t length = vSize(next_edge->to_->p_ - next_edge->from_->p_);
            should_dissolve |= filterEndOfCentralTransition(next_edge, traveled_dist + length, max_dist, replacing_bead_count);
            is_end_of_central = false;
        }
    }
    if (is_end_of_central && traveled_dist < max_dist)
    {
        should_dissolve = true;
    }

    if (should_dissolve)
    {
        edge_to_start->to_->data_.bead_count_ = replacing_bead_count;
    }
    return should_dissolve;
}

void SkeletalTrapezoidation::generateAllTransitionEnds(ptr_vector_t<std::list<TransitionEnd>>& edge_transition_ends)
{
    for (edge_t& edge : graph_.edges)
    {
        if (! edge.data_.hasTransitions())
        {
            continue;
        }
        auto& transition_positions = *edge.data_.getTransitions();

        // assert(edge.from_->data_.distance_to_boundary_ <= edge.to_->data_.distance_to_boundary_); // 原始断言，注释保留以备回滚
        if (edge.from_->data_.distance_to_boundary_ > edge.to_->data_.distance_to_boundary_)
        {
            CURA_ERROR("BeadingStrategy错误: 边缘起点到边界距离大于终点距离");
            CURA_ERROR("  - from_distance: {}, to_distance: {}", edge.from_->data_.distance_to_boundary_, edge.to_->data_.distance_to_boundary_);
            CURA_ERROR("  - edge位置: from({}, {}) to({}, {})",
                      edge.from_->p_.X, edge.from_->p_.Y, edge.to_->p_.X, edge.to_->p_.Y);
            CURA_ERROR("  - 期望: from_distance <= to_distance");
            CURA_ERROR("  - 可能原因: 距离计算错误或边缘方向错误");
            continue; // 跳过这个有问题的边缘
        }

        for (TransitionMiddle& transition_middle : transition_positions)
        {
            // assert(transition_positions.front().pos_ <= transition_middle.pos_); // 原始断言，注释保留以备回滚
            // assert(transition_middle.pos_ <= transition_positions.back().pos_); // 原始断言，注释保留以备回滚
            if (transition_positions.front().pos_ > transition_middle.pos_ ||
                transition_middle.pos_ > transition_positions.back().pos_)
            {
                CURA_ERROR("BeadingStrategy错误: 过渡位置不在有效范围内");
                CURA_ERROR("  - front_pos: {}, middle_pos: {}, back_pos: {}",
                          transition_positions.front().pos_, transition_middle.pos_, transition_positions.back().pos_);
                CURA_ERROR("  - lower_bead_count: {}", transition_middle.lower_bead_count_);
                CURA_ERROR("  - 期望: front_pos <= middle_pos <= back_pos");
                CURA_ERROR("  - 可能原因: 过渡位置计算错误或排序问题");
                continue; // 跳过这个无效的过渡
            }
            generateTransitionEnds(edge, transition_middle.pos_, transition_middle.lower_bead_count_, edge_transition_ends);
        }
    }
}

void SkeletalTrapezoidation::generateTransitionEnds(edge_t& edge, coord_t mid_pos, coord_t lower_bead_count, ptr_vector_t<std::list<TransitionEnd>>& edge_transition_ends)
{
    CURA_ERROR_FLUSH_F("=== 生成过渡端点 ===");

    const Point2LL a = edge.from_->p_;
    const Point2LL b = edge.to_->p_;
    const Point2LL ab = b - a;
    const coord_t ab_size = vSize(ab);

    // 验证输入参数
    if (!CURA_CHECK_WITH_ERROR(ab_size > 0, "边缘长度为零或负数"))
    {
        CURA_ERROR_FLUSH_F("无效边缘: ab_size=%d, 位置from(%d,%d) to(%d,%d)",
                          ab_size, a.X, a.Y, b.X, b.Y);
        return;
    }

    if (!CURA_CHECK_WITH_ERROR(mid_pos >= 0 && mid_pos <= ab_size, "mid_pos超出边缘范围"))
    {
        CURA_ERROR_FLUSH_F("mid_pos超出范围: mid_pos=%d, ab_size=%d", mid_pos, ab_size);
        return;
    }

    const coord_t transition_length = beading_strategy_.getTransitioningLength(lower_bead_count);
    const double transition_mid_position = beading_strategy_.getTransitionAnchorPos(lower_bead_count);
    constexpr double inner_bead_width_ratio_after_transition = 1.0;

    CURA_ERROR_FLUSH_F("过渡参数: mid_pos=%d, lower_bead_count=%d, transition_length=%d, mid_position=%.3f",
                      mid_pos, lower_bead_count, transition_length, transition_mid_position);

    constexpr Ratio start_rest{ 0.0 };
    const double mid_rest = transition_mid_position * inner_bead_width_ratio_after_transition;
    constexpr double end_rest = inner_bead_width_ratio_after_transition;

    { // Lower bead count transition end
        const coord_t start_pos = ab_size - mid_pos;
        const coord_t transition_half_length = transition_mid_position * transition_length;
        const coord_t end_pos = start_pos + transition_half_length;

        CURA_ERROR_FLUSH_F("下端过渡: start_pos=%d, end_pos=%d, half_length=%d",
                          start_pos, end_pos, transition_half_length);

        // 验证twin边缘存在
        if (!CURA_CHECK_WITH_ERROR(edge.twin_, "边缘缺少twin"))
        {
            CURA_ERROR_FLUSH_F("边缘缺少twin: edge地址=%p", static_cast<void*>(&edge));
            return;
        }

        generateTransitionEnd(*edge.twin_, start_pos, end_pos, transition_half_length, mid_rest, start_rest, lower_bead_count, edge_transition_ends);
    }

    { // Upper bead count transition end
        const coord_t start_pos = mid_pos;
        const coord_t transition_half_length = (1.0 - transition_mid_position) * transition_length;
        const coord_t end_pos = mid_pos + transition_half_length;

        CURA_ERROR_FLUSH_F("上端过渡: start_pos=%d, end_pos=%d, half_length=%d",
                          start_pos, end_pos, transition_half_length);

        bool success = generateTransitionEnd(edge, start_pos, end_pos, transition_half_length, mid_rest, end_rest, lower_bead_count, edge_transition_ends);

        if (!success)
        {
            CURA_ERROR_FLUSH("警告: 上端过渡生成失败，但这在某些情况下是正常的");
            CURA_ERROR_FLUSH("原因: 可能没有足够的bead计数增长来支持过渡");
        }
    }
}

bool SkeletalTrapezoidation::generateTransitionEnd(
    edge_t& edge,
    coord_t start_pos,
    coord_t end_pos,
    coord_t transition_half_length,
    Ratio start_rest,
    Ratio end_rest,
    coord_t lower_bead_count,
    ptr_vector_t<std::list<TransitionEnd>>& edge_transition_ends)
{
    CURA_ERROR_FLUSH_F("--- generateTransitionEnd ---");

    Point2LL a = edge.from_->p_;
    Point2LL b = edge.to_->p_;
    Point2LL ab = b - a;
    coord_t ab_size = vSize(ab); // TODO: prevent recalculation of these values

    CURA_ERROR_FLUSH_F("输入参数: start_pos=%d, end_pos=%d, half_length=%d, ab_size=%d",
                      start_pos, end_pos, transition_half_length, ab_size);
    CURA_ERROR_FLUSH_F("Rest值: start_rest=%.3f, end_rest=%.3f, lower_bead_count=%d",
                      static_cast<double>(start_rest), static_cast<double>(end_rest), lower_bead_count);

    // 使用崩溃安全检查起始位置
    if (!CURA_CHECK_WITH_ERROR(start_pos <= ab_size, "边缘起始位置超出边缘范围"))
    {
        CURA_ERROR_FLUSH_F("位置超出范围:");
        CURA_ERROR_FLUSH_F("  - start_pos: %d, ab_size: %d (超出: %d)", start_pos, ab_size, start_pos - ab_size);
        CURA_ERROR_FLUSH_F("  - end_pos: %d, transition_half_length: %d", end_pos, transition_half_length);
        CURA_ERROR_FLUSH_F("  - edge位置: from(%d,%d) to(%d,%d)", a.X, a.Y, b.X, b.Y);
        CURA_ERROR_FLUSH("  - 可能原因: 位置计算错误或边缘长度计算问题");
        return false;
    }

    bool going_up = end_rest > start_rest;
    CURA_ERROR_FLUSH_F("过渡方向: going_up=%s", going_up ? "true" : "false");

    // 使用崩溃安全检查central状态
    if (!CURA_CHECK_WITH_ERROR(edge.data_.isCentral(), "尝试在非中心区域生成过渡端点"))
    {
        CURA_ERROR_FLUSH_F("非中心区域错误:");
        CURA_ERROR_FLUSH_F("  - edge.data_.isCentral(): %s", edge.data_.isCentral() ? "true" : "false");
        CURA_ERROR_FLUSH_F("  - edge位置: from(%d,%d) to(%d,%d)", a.X, a.Y, b.X, b.Y);
        CURA_ERROR_FLUSH_F("  - edge地址: %p", static_cast<void*>(&edge));
        CURA_ERROR_FLUSH("  - 可能原因: central状态设置错误或函数调用错误");
        return false;
    }

    if (end_pos > ab_size)
    { // Recurse on all further edges
        CURA_ERROR_FLUSH_F("过渡超出边缘，需要递归: end_pos=%d > ab_size=%d", end_pos, ab_size);

        // 验证除法安全性
        coord_t denominator = start_pos - end_pos;
        if (!CURA_CHECK_WITH_ERROR(denominator != 0, "rest值计算中分母为零"))
        {
            CURA_ERROR_FLUSH_F("除零错误: start_pos=%d, end_pos=%d", start_pos, end_pos);
            return false;
        }

        double rest = end_rest - (start_rest - end_rest) * (end_pos - ab_size) / denominator;

        double min_rest = std::min(static_cast<double>(end_rest), static_cast<double>(start_rest));
        double max_rest = std::max(static_cast<double>(end_rest), static_cast<double>(start_rest));

        CURA_ERROR_FLUSH_F("Rest值计算: rest=%.6f, 范围[%.6f, %.6f]", rest, min_rest, max_rest);

        // 使用崩溃安全检查rest值范围
        if (!CURA_CHECK_WITH_ERROR(rest >= min_rest - 1e-6 && rest <= max_rest + 1e-6, "计算的rest值超出有效范围"))
        {
            CURA_ERROR_FLUSH_F("Rest值超出范围:");
            CURA_ERROR_FLUSH_F("  - rest: %.6f", rest);
            CURA_ERROR_FLUSH_F("  - min_rest: %.6f, max_rest: %.6f", min_rest, max_rest);
            CURA_ERROR_FLUSH_F("  - start_rest: %.6f, end_rest: %.6f",
                              static_cast<double>(start_rest), static_cast<double>(end_rest));
            CURA_ERROR_FLUSH_F("  - start_pos: %d, end_pos: %d, ab_size: %d", start_pos, end_pos, ab_size);
            CURA_ERROR_FLUSH_F("  - 分子: %.6f, 分母: %d",
                              (static_cast<double>(start_rest) - static_cast<double>(end_rest)) * (end_pos - ab_size), denominator);
            CURA_ERROR_FLUSH("  - 计算公式: end_rest - (start_rest - end_rest) * (end_pos - ab_size) / (start_pos - end_pos)");
            CURA_ERROR_FLUSH("  - 可能原因: 数值精度问题或位置计算错误");

            // 如果差异很大，可能是严重错误
            if (rest < min_rest - 0.1 || rest > max_rest + 0.1)
            {
                CURA_ASSERT_WITH_INFO(false, "Rest值严重超出范围，可能存在计算错误");
            }
            return false;
        }

        coord_t central_edge_count = 0;
        for (edge_t* outgoing = edge.next_; outgoing && outgoing != edge.twin_; outgoing = outgoing->twin_->next_)
        {
            if (! outgoing->data_.isCentral())
                continue;
            central_edge_count++;
        }

        bool is_only_going_down = true;
        bool has_recursed = false;
        for (edge_t* outgoing = edge.next_; outgoing && outgoing != edge.twin_;)
        {
            edge_t* next = outgoing->twin_->next_; // Before we change the outgoing edge itself
            if (! outgoing->data_.isCentral())
            {
                outgoing = next;
                continue; // Don't put transition ends in non-central regions
            }
            if (central_edge_count > 1 && going_up && isGoingDown(outgoing, 0, end_pos - ab_size + transition_half_length, lower_bead_count))
            { // We're after a 3-way_all-central_junction-node and going in the direction of lower bead count
                // don't introduce a transition end along this central direction, because this direction is the downward direction
                // while we are supposed to be [going_up]
                outgoing = next;
                continue;
            }
            bool is_going_down = generateTransitionEnd(*outgoing, 0, end_pos - ab_size, transition_half_length, rest, end_rest, lower_bead_count, edge_transition_ends);
            is_only_going_down &= is_going_down;
            outgoing = next;
            has_recursed = true;
        }
        if (! going_up || (has_recursed && ! is_only_going_down))
        {
            edge.to_->data_.transition_ratio_ = rest;
            edge.to_->data_.bead_count_ = lower_bead_count;
        }
        return is_only_going_down;
    }
    else // end_pos < ab_size
    { // Add transition end point here
        bool is_lower_end = end_rest == 0; // TODO collapse this parameter into the bool for which it is used here!
        coord_t pos = -1;

        edge_t* upward_edge = nullptr;
        if (edge.isUpward())
        {
            upward_edge = &edge;
            pos = end_pos;
        }
        else
        {
            upward_edge = edge.twin_;
            pos = ab_size - end_pos;
        }

        if (! upward_edge->data_.hasTransitionEnds())
        {
            // This edge doesn't have a data structure yet for the transition ends. Make one.
            edge_transition_ends.emplace_back(std::make_shared<std::list<TransitionEnd>>());
            upward_edge->data_.setTransitionEnds(edge_transition_ends.back());
        }
        auto transitions = upward_edge->data_.getTransitionEnds();

        // Add a transition to it (on the correct side).
        assert(ab_size == vSize(edge.twin_->from_->p_ - edge.twin_->to_->p_));
        assert(pos <= ab_size);
        if (transitions->empty() || pos < transitions->front().pos_)
        { // Preorder so that sorting later on is faster
            transitions->emplace_front(pos, lower_bead_count, is_lower_end);
        }
        else
        {
            transitions->emplace_back(pos, lower_bead_count, is_lower_end);
        }
        return false;
    }
}


bool SkeletalTrapezoidation::isGoingDown(edge_t* outgoing, coord_t traveled_dist, coord_t max_dist, coord_t lower_bead_count) const
{
    // NOTE: the logic below is not fully thought through.
    // TODO: take transition mids into account
    if (outgoing->to_->data_.distance_to_boundary_ == 0)
    {
        return true;
    }
    bool is_upward = outgoing->to_->data_.distance_to_boundary_ >= outgoing->from_->data_.distance_to_boundary_;
    edge_t* upward_edge = is_upward ? outgoing : outgoing->twin_;
    if (outgoing->to_->data_.bead_count_ > lower_bead_count + 1)
    {
        assert(upward_edge->data_.hasTransitions() && "If the bead count is going down there has to be a transition mid!");
        if (! upward_edge->data_.hasTransitions())
        {
            spdlog::warn("If the bead count is going down there has to be a transition mid!");
        }
        return false;
    }
    coord_t length = vSize(outgoing->to_->p_ - outgoing->from_->p_);
    if (upward_edge->data_.hasTransitions())
    {
        auto& transition_mids = *upward_edge->data_.getTransitions();
        TransitionMiddle& mid = is_upward ? transition_mids.front() : transition_mids.back();
        if (mid.lower_bead_count_ == lower_bead_count && ((is_upward && mid.pos_ + traveled_dist < max_dist) || (! is_upward && length - mid.pos_ + traveled_dist < max_dist)))
        {
            return true;
        }
    }
    if (traveled_dist + length > max_dist)
    {
        return false;
    }
    if (outgoing->to_->data_.bead_count_ <= lower_bead_count && ! (outgoing->to_->data_.bead_count_ == lower_bead_count && outgoing->to_->data_.transition_ratio_ > 0.0))
    {
        return true;
    }

    bool is_only_going_down = true;
    bool has_recursed = false;
    for (edge_t* next = outgoing->next_; next && next != outgoing->twin_; next = next->twin_->next_)
    {
        if (! next->data_.isCentral())
        {
            continue;
        }
        bool is_going_down = isGoingDown(next, traveled_dist + length, max_dist, lower_bead_count);
        is_only_going_down &= is_going_down;
        has_recursed = true;
    }
    return has_recursed && is_only_going_down;
}

void SkeletalTrapezoidation::applyTransitions(ptr_vector_t<std::list<TransitionEnd>>& edge_transition_ends)
{
    for (edge_t& edge : graph_.edges)
    {
        if (edge.twin_->data_.hasTransitionEnds())
        {
            coord_t length = vSize(edge.from_->p_ - edge.to_->p_);
            auto& twin_transition_ends = *edge.twin_->data_.getTransitionEnds();
            if (! edge.data_.hasTransitionEnds())
            {
                edge_transition_ends.emplace_back(std::make_shared<std::list<TransitionEnd>>());
                edge.data_.setTransitionEnds(edge_transition_ends.back());
            }
            auto& transition_ends = *edge.data_.getTransitionEnds();
            for (TransitionEnd& end : twin_transition_ends)
            {
                transition_ends.emplace_back(length - end.pos_, end.lower_bead_count_, end.is_lower_end_);
            }
            twin_transition_ends.clear();
        }
    }

    for (edge_t& edge : graph_.edges)
    {
        if (! edge.data_.hasTransitionEnds())
        {
            continue;
        }

        assert(edge.data_.isCentral());

        auto& transitions = *edge.data_.getTransitionEnds();
        transitions.sort(
            [](const TransitionEnd& a, const TransitionEnd& b)
            {
                return a.pos_ < b.pos_;
            });

        node_t* from = edge.from_;
        node_t* to = edge.to_;
        Point2LL a = from->p_;
        Point2LL b = to->p_;
        Point2LL ab = b - a;
        coord_t ab_size = vSize(ab);

        edge_t* last_edge_replacing_input = &edge;
        for (TransitionEnd& transition_end : transitions)
        {
            coord_t new_node_bead_count = transition_end.is_lower_end_ ? transition_end.lower_bead_count_ : transition_end.lower_bead_count_ + 1;
            coord_t end_pos = transition_end.pos_;
            node_t* close_node = (end_pos < ab_size / 2) ? from : to;
            if ((end_pos < snap_dist_ || end_pos > ab_size - snap_dist_) && close_node->data_.bead_count_ == new_node_bead_count)
            {
                assert(end_pos <= ab_size);
                close_node->data_.transition_ratio_ = 0;
                continue;
            }
            Point2LL mid = a + normal(ab, end_pos);

            assert(last_edge_replacing_input->data_.isCentral());
            assert(last_edge_replacing_input->data_.type_ != SkeletalTrapezoidationEdge::EdgeType::EXTRA_VD);
            last_edge_replacing_input = graph_.insertNode(last_edge_replacing_input, mid, new_node_bead_count);
            assert(last_edge_replacing_input->data_.type_ != SkeletalTrapezoidationEdge::EdgeType::EXTRA_VD);
            assert(last_edge_replacing_input->data_.isCentral());
        }
    }
}

bool SkeletalTrapezoidation::isEndOfCentral(const edge_t& edge_to) const
{
    if (! edge_to.data_.isCentral())
    {
        return false;
    }
    if (! edge_to.next_)
    {
        return true;
    }
    for (const edge_t* edge = edge_to.next_; edge && edge != edge_to.twin_; edge = edge->twin_->next_)
    {
        if (edge->data_.isCentral())
        {
            return false;
        }
        assert(edge->twin_);
    }
    return true;
}

void SkeletalTrapezoidation::generateExtraRibs()
{
    // NOTE: At one point there was a comment here and some odd code that seemed to suggest some edge(s?) at the end should perhaps not be looped over.
    //       The code was equivalent to a full loop over all the edges though, unless there was one edge or less, in which case it would produce undefined behaviour.
    for (auto& edge : graph_.edges)
    {
        if (! edge.data_.isCentral() || shorterThen(edge.to_->p_ - edge.from_->p_, discretization_step_size_)
            || edge.from_->data_.distance_to_boundary_ >= edge.to_->data_.distance_to_boundary_)
        {
            continue;
        }

        std::vector<coord_t> rib_thicknesses = beading_strategy_.getNonlinearThicknesses(edge.from_->data_.bead_count_);

        if (rib_thicknesses.empty())
        {
            continue;
        }

        // Preload some variables before [edge] gets changed
        node_t* from = edge.from_;
        node_t* to = edge.to_;
        Point2LL a = from->p_;
        Point2LL b = to->p_;
        Point2LL ab = b - a;
        coord_t ab_size = vSize(ab);
        coord_t a_R = edge.from_->data_.distance_to_boundary_;
        coord_t b_R = edge.to_->data_.distance_to_boundary_;

        edge_t* last_edge_replacing_input = &edge;
        for (coord_t rib_thickness : rib_thicknesses)
        {
            if (rib_thickness / 2 <= a_R)
            {
                continue;
            }
            if (rib_thickness / 2 >= b_R)
            {
                break;
            }

            coord_t new_node_bead_count = std::min(edge.from_->data_.bead_count_, edge.to_->data_.bead_count_);
            coord_t end_pos = ab_size * (rib_thickness / 2 - a_R) / (b_R - a_R);
            assert(end_pos > 0);
            assert(end_pos < ab_size);
            node_t* close_node = (end_pos < ab_size / 2) ? from : to;
            if ((end_pos < snap_dist_ || end_pos > ab_size - snap_dist_) && close_node->data_.bead_count_ == new_node_bead_count)
            {
                assert(end_pos <= ab_size);
                close_node->data_.transition_ratio_ = 0;
                continue;
            }
            Point2LL mid = a + normal(ab, end_pos);

            assert(last_edge_replacing_input->data_.isCentral());
            assert(last_edge_replacing_input->data_.type_ != SkeletalTrapezoidationEdge::EdgeType::EXTRA_VD);
            last_edge_replacing_input = graph_.insertNode(last_edge_replacing_input, mid, new_node_bead_count);
            assert(last_edge_replacing_input->data_.type_ != SkeletalTrapezoidationEdge::EdgeType::EXTRA_VD);
            assert(last_edge_replacing_input->data_.isCentral());
        }
    }
}

//
// ^^^^^^^^^^^^^^^^^^^^^
//    TRANSITIONING
// =====================
//  TOOLPATH GENERATION
// vvvvvvvvvvvvvvvvvvvvv
//

void SkeletalTrapezoidation::generateSegments()
{
    CURA_ERROR_FLUSH("=== 开始生成线段 ===");

    // 在生成线段前验证和修复图完整性
    validateAndFixGraphIntegrity();

    std::vector<edge_t*> upward_quad_mids;
    for (edge_t& edge : graph_.edges)
    {
        if (edge.prev_ && edge.next_ && edge.isUpward())
        {
            upward_quad_mids.emplace_back(&edge);
        }
    }

    std::stable_sort(
        upward_quad_mids.begin(),
        upward_quad_mids.end(),
        [this](edge_t* a, edge_t* b)
        {
            if (a->to_->data_.distance_to_boundary_ == b->to_->data_.distance_to_boundary_)
            { // PathOrdering between two 'upward' edges of the same distance is important when one of the edges is flat and connected to the other
                if (a->from_->data_.distance_to_boundary_ == a->to_->data_.distance_to_boundary_ && b->from_->data_.distance_to_boundary_ == b->to_->data_.distance_to_boundary_)
                {
                    coord_t max = std::numeric_limits<coord_t>::max();
                    coord_t a_dist_from_up = std::min(a->distToGoUp().value_or(max), a->twin_->distToGoUp().value_or(max)) - vSize(a->to_->p_ - a->from_->p_);
                    coord_t b_dist_from_up = std::min(b->distToGoUp().value_or(max), b->twin_->distToGoUp().value_or(max)) - vSize(b->to_->p_ - b->from_->p_);
                    return a_dist_from_up < b_dist_from_up;
                }
                else if (a->from_->data_.distance_to_boundary_ == a->to_->data_.distance_to_boundary_)
                {
                    return true; // Edge a might be 'above' edge b
                }
                else if (b->from_->data_.distance_to_boundary_ == b->to_->data_.distance_to_boundary_)
                {
                    return false; // Edge b might be 'above' edge a
                }
                else
                {
                    // PathOrdering is not important
                }
            }
            return a->to_->data_.distance_to_boundary_ > b->to_->data_.distance_to_boundary_;
        });

    ptr_vector_t<BeadingPropagation> node_beadings;
    { // Store beading
        for (node_t& node : graph_.nodes)
        {
            if (node.data_.bead_count_ <= 0)
            {
                continue;
            }
            if (node.data_.transition_ratio_ == 0)
            {
                node_beadings.emplace_back(new BeadingPropagation(beading_strategy_.compute(node.data_.distance_to_boundary_ * 2, node.data_.bead_count_)));
                node.data_.setBeading(node_beadings.back());
                assert(node_beadings.back()->beading_.total_thickness == node.data_.distance_to_boundary_ * 2);
                if (node_beadings.back()->beading_.total_thickness != node.data_.distance_to_boundary_ * 2)
                {
                    spdlog::warn("If transitioning to an endpoint (ratio 0), the node should be exactly in the middle.");
                }
            }
            else
            {
                Beading low_count_beading = beading_strategy_.compute(node.data_.distance_to_boundary_ * 2, node.data_.bead_count_);
                Beading high_count_beading = beading_strategy_.compute(node.data_.distance_to_boundary_ * 2, node.data_.bead_count_ + 1);
                Beading merged = interpolate(low_count_beading, 1.0 - node.data_.transition_ratio_, high_count_beading);
                node_beadings.emplace_back(new BeadingPropagation(merged));
                node.data_.setBeading(node_beadings.back());
                assert(merged.total_thickness == node.data_.distance_to_boundary_ * 2);
                if (merged.total_thickness != node.data_.distance_to_boundary_ * 2)
                {
                    spdlog::warn("If merging two beads, the new bead must be exactly in the middle.");
                }
            }
        }
    }

    propagateBeadingsUpward(upward_quad_mids, node_beadings);

    propagateBeadingsDownward(upward_quad_mids, node_beadings);

    ptr_vector_t<LineJunctions> edge_junctions; // junctions ordered high R to low R
    generateJunctions(node_beadings, edge_junctions);

    connectJunctions(edge_junctions);

    generateLocalMaximaSingleBeads();
}

SkeletalTrapezoidation::edge_t* SkeletalTrapezoidation::getQuadMaxRedgeTo(edge_t* quad_start_edge)
{
    // 使用崩溃安全检查替代assert
    if (!CURA_CHECK_WITH_ERROR(quad_start_edge != nullptr, "quad_start_edge为空"))
    {
        return nullptr;
    }

    if (!CURA_CHECK_WITH_ERROR(quad_start_edge->prev_ == nullptr, "quad_start_edge->prev_不为空"))
    {
        CURA_ERROR_FLUSH_F("quad_start_edge位置: (%d,%d)",
                          quad_start_edge->from_->p_.X, quad_start_edge->from_->p_.Y);
        return nullptr;
    }

    if (!CURA_CHECK_WITH_ERROR(quad_start_edge->from_ != nullptr &&
        quad_start_edge->from_->data_.distance_to_boundary_ == 0, "起始边缘距离边界不为0"))
    {
        CURA_ERROR_FLUSH_F("quad_start_edge->from_距离: %d",
                          quad_start_edge->from_->data_.distance_to_boundary_);
        return nullptr;
    }

    coord_t max_R = -1;
    edge_t* ret = nullptr;
    size_t edge_count = 0;

    for (edge_t* edge = quad_start_edge; edge; edge = edge->next_)
    {
        edge_count++;

        // 防止无限循环
        if (edge_count > 10000)
        {
            CURA_ERROR_FLUSH("getQuadMaxRedgeTo循环超过10000次，可能存在无限循环");
            break;
        }

        if (!CURA_CHECK_WITH_ERROR(edge->to_ != nullptr, "边缘的to_节点为空"))
        {
            CURA_ERROR_FLUSH_F("边缘位置: from(%d,%d)",
                              edge->from_->p_.X, edge->from_->p_.Y);
            break;
        }

        coord_t r = edge->to_->data_.distance_to_boundary_;
        if (r > max_R)
        {
            max_R = r;
            ret = edge;
        }
    }

    if (!CURA_CHECK_WITH_ERROR(ret != nullptr, "未找到有效的最大R边缘"))
    {
        return nullptr;
    }

    // 安全检查ret->next_和ret->prev_
    if (!ret->next_ && ret->to_->data_.distance_to_boundary_ - 5 < ret->from_->data_.distance_to_boundary_)
    {
        if (CURA_CHECK_WITH_ERROR(ret->prev_ != nullptr, "ret->prev_为空，无法回退"))
        {
            ret = ret->prev_;
        }
        else
        {
            CURA_ERROR_FLUSH("无法回退到prev_，返回当前ret");
        }
    }

    if (!CURA_CHECK_WITH_ERROR(ret != nullptr, "最终ret为空"))
    {
        return nullptr;
    }

    if (!CURA_CHECK_WITH_ERROR(ret->next_ != nullptr, "ret->next_为空"))
    {
        CURA_ERROR_FLUSH_F("ret位置: (%d,%d)", ret->to_->p_.X, ret->to_->p_.Y);
        return nullptr;
    }

    return ret;
}

void SkeletalTrapezoidation::propagateBeadingsUpward(std::vector<edge_t*>& upward_quad_mids, ptr_vector_t<BeadingPropagation>& node_beadings)
{
    for (auto upward_quad_mids_it = upward_quad_mids.rbegin(); upward_quad_mids_it != upward_quad_mids.rend(); ++upward_quad_mids_it)
    {
        edge_t* upward_edge = *upward_quad_mids_it;
        if (upward_edge->to_->data_.bead_count_ >= 0)
        { // Don't override local beading
            continue;
        }
        if (! upward_edge->from_->data_.hasBeading())
        { // Only propagate if we have something to propagate
            continue;
        }
        BeadingPropagation& lower_beading = *upward_edge->from_->data_.getBeading();
        if (upward_edge->to_->data_.hasBeading())
        { // Only propagate to places where there is place
            continue;
        }
        assert(
            (upward_edge->from_->data_.distance_to_boundary_ != upward_edge->to_->data_.distance_to_boundary_
             || shorterThen(upward_edge->to_->p_ - upward_edge->from_->p_, central_filter_dist_))
            && "zero difference R edges should always be central");
        coord_t length = vSize(upward_edge->to_->p_ - upward_edge->from_->p_);
        BeadingPropagation upper_beading = lower_beading;
        upper_beading.dist_to_bottom_source_ += length;
        upper_beading.is_upward_propagated_only_ = true;
        node_beadings.emplace_back(new BeadingPropagation(upper_beading));
        upward_edge->to_->data_.setBeading(node_beadings.back());
        assert(upper_beading.beading_.total_thickness <= upward_edge->to_->data_.distance_to_boundary_ * 2);
    }
}

void SkeletalTrapezoidation::propagateBeadingsDownward(std::vector<edge_t*>& upward_quad_mids, ptr_vector_t<BeadingPropagation>& node_beadings)
{
    for (edge_t* upward_quad_mid : upward_quad_mids)
    {
        // Transfer beading information to lower nodes
        if (! upward_quad_mid->data_.isCentral())
        {
            // for equidistant edge: propagate from known beading to node with unknown beading
            if (upward_quad_mid->from_->data_.distance_to_boundary_ == upward_quad_mid->to_->data_.distance_to_boundary_ && upward_quad_mid->from_->data_.hasBeading()
                && ! upward_quad_mid->to_->data_.hasBeading())
            {
                propagateBeadingsDownward(upward_quad_mid->twin_, node_beadings);
            }
            else
            {
                propagateBeadingsDownward(upward_quad_mid, node_beadings);
            }
        }
    }
}

void SkeletalTrapezoidation::propagateBeadingsDownward(edge_t* edge_to_peak, ptr_vector_t<BeadingPropagation>& node_beadings)
{
    // 进一步减少调试输出，避免多线程混乱
    static thread_local size_t call_count = 0;
    call_count++;
    if (call_count <= 1 || call_count % 10000 == 0)  // 只记录第1次和每10000次
    {
        CURA_ERROR_FLUSH_F("=== propagateBeadingsDownward #%zu ===", call_count);
    }

    // 验证输入参数
    if (!CURA_CHECK_WITH_ERROR(edge_to_peak != nullptr, "edge_to_peak为空指针"))
    {
        CURA_ERROR_FLUSH("edge_to_peak为空，无法传播beading");
        return;
    }

    if (!CURA_CHECK_WITH_ERROR(edge_to_peak->to_ != nullptr && edge_to_peak->from_ != nullptr,
        "边缘的to_或from_节点为空"))
    {
        CURA_ERROR_FLUSH_F("边缘节点为空: edge=%p", static_cast<void*>(edge_to_peak));
        return;
    }

    coord_t length = vSize(edge_to_peak->to_->p_ - edge_to_peak->from_->p_);
    CURA_ERROR_FLUSH_F("边缘长度: %d, from(%d,%d) to(%d,%d)",
                      length, edge_to_peak->from_->p_.X, edge_to_peak->from_->p_.Y,
                      edge_to_peak->to_->p_.X, edge_to_peak->to_->p_.Y);

    auto top_beading_ptr = getOrCreateBeading(edge_to_peak->to_, node_beadings);
    if (!CURA_CHECK_WITH_ERROR(top_beading_ptr != nullptr, "获取top_beading失败"))
    {
        CURA_ERROR_FLUSH("无法获取top_beading，停止传播");
        return;
    }

    BeadingPropagation& top_beading = *top_beading_ptr;

    // 使用崩溃安全检查替代assert
    coord_t expected_thickness = edge_to_peak->to_->data_.distance_to_boundary_ * 2;
    if (!CURA_CHECK_WITH_ERROR(top_beading.beading_.total_thickness >= expected_thickness,
        "Top bead厚度小于期望值"))
    {
        CURA_ERROR_FLUSH_F("Top bead厚度检查失败:");
        CURA_ERROR_FLUSH_F("  - total_thickness: %d", top_beading.beading_.total_thickness);
        CURA_ERROR_FLUSH_F("  - expected (distance*2): %d", expected_thickness);
        CURA_ERROR_FLUSH_F("  - distance_to_boundary: %d", edge_to_peak->to_->data_.distance_to_boundary_);
        spdlog::warn("Top bead is beyond the center of the total width.");
    }

    if (!CURA_CHECK_WITH_ERROR(!top_beading.is_upward_propagated_only_,
        "Top beading只能向上传播"))
    {
        CURA_ERROR_FLUSH("Top beading传播方向错误");
        // 修正传播方向
        top_beading.is_upward_propagated_only_ = false;
    }

    if (! edge_to_peak->from_->data_.hasBeading())
    { // Set new beading if there is no beading associated with the node yet
        BeadingPropagation propagated_beading = top_beading;
        propagated_beading.dist_from_top_source_ += length;
        node_beadings.emplace_back(new BeadingPropagation(propagated_beading));
        edge_to_peak->from_->data_.setBeading(node_beadings.back());
        // 使用崩溃安全检查替代assert
        coord_t expected_propagated_thickness = edge_to_peak->from_->data_.distance_to_boundary_ * 2;
        if (!CURA_CHECK_WITH_ERROR(propagated_beading.beading_.total_thickness >= expected_propagated_thickness,
            "传播的bead厚度小于期望值"))
        {
            CURA_ERROR_FLUSH_F("传播bead厚度检查失败:");
            CURA_ERROR_FLUSH_F("  - total_thickness: %d", propagated_beading.beading_.total_thickness);
            CURA_ERROR_FLUSH_F("  - expected (distance*2): %d", expected_propagated_thickness);
            CURA_ERROR_FLUSH_F("  - from_distance_to_boundary: %d", edge_to_peak->from_->data_.distance_to_boundary_);
            spdlog::warn("Propagated bead is beyond the center of the total width.");
        }
    }
    else
    {
        BeadingPropagation& bottom_beading = *edge_to_peak->from_->data_.getBeading();
        coord_t total_dist = top_beading.dist_from_top_source_ + length + bottom_beading.dist_to_bottom_source_;
        Ratio ratio_of_top = static_cast<double>(bottom_beading.dist_to_bottom_source_) / std::min(total_dist, beading_propagation_transition_dist_);
        ratio_of_top = std::max(0.0_r, ratio_of_top);
        if (ratio_of_top >= 1.0)
        {
            bottom_beading = top_beading;
            bottom_beading.dist_from_top_source_ += length;
        }
        else
        {
            Beading merged_beading = interpolate(top_beading.beading_, ratio_of_top, bottom_beading.beading_, edge_to_peak->from_->data_.distance_to_boundary_);
            bottom_beading = BeadingPropagation(merged_beading);
            bottom_beading.is_upward_propagated_only_ = false;
            assert(merged_beading.total_thickness >= edge_to_peak->from_->data_.distance_to_boundary_ * 2);
            if (merged_beading.total_thickness < edge_to_peak->from_->data_.distance_to_boundary_ * 2)
            {
                spdlog::warn("Merged bead is beyond the center of the total width.");
            }
        }
    }
}


SkeletalTrapezoidation::Beading SkeletalTrapezoidation::interpolate(const Beading& left, Ratio ratio_left_to_whole, const Beading& right, coord_t switching_radius) const
{
    assert(ratio_left_to_whole >= 0.0 && ratio_left_to_whole <= 1.0);
    Beading ret = interpolate(left, ratio_left_to_whole, right);

    // TODO: don't use toolpath locations past the middle!
    // TODO: stretch bead widths and locations of the higher bead count beading to fit in the left over space
    coord_t next_inset_idx;
    for (next_inset_idx = static_cast<coord_t>(left.toolpath_locations.size()) - 1; next_inset_idx >= 0; next_inset_idx--)
    {
        if (switching_radius > left.toolpath_locations[next_inset_idx])
        {
            break;
        }
    }
    if (next_inset_idx < 0)
    { // There is no next inset, because there is only one
        assert(left.toolpath_locations.empty() || left.toolpath_locations.front() >= switching_radius);
        return ret;
    }
    if (next_inset_idx + 1 == coord_t(left.toolpath_locations.size()))
    { // We cant adjust to fit the next edge because there is no previous one?!
        return ret;
    }
    assert(next_inset_idx < coord_t(left.toolpath_locations.size()));
    assert(left.toolpath_locations[next_inset_idx] <= switching_radius);
    assert(left.toolpath_locations[next_inset_idx + 1] >= switching_radius);
    if (ret.toolpath_locations[next_inset_idx] > switching_radius)
    { // One inset disappeared between left and the merged one
        // solve for ratio f:
        // f*l + (1-f)*r = s
        // f*l + r - f*r = s
        // f*(l-r) + r = s
        // f*(l-r) = s - r
        // f = (s-r) / (l-r)
        double new_ratio = static_cast<double>(switching_radius - right.toolpath_locations[next_inset_idx])
                         / static_cast<double>(left.toolpath_locations[next_inset_idx] - right.toolpath_locations[next_inset_idx]);
        new_ratio = std::min(1.0, new_ratio + 0.1);
        return interpolate(left, new_ratio, right);
    }
    return ret;
}


SkeletalTrapezoidation::Beading SkeletalTrapezoidation::interpolate(const Beading& left, Ratio ratio_left_to_whole, const Beading& right) const
{
    assert(ratio_left_to_whole >= 0.0 && ratio_left_to_whole <= 1.0);
    double ratio_right_to_whole = 1.0 - ratio_left_to_whole;

    Beading ret = (left.total_thickness > right.total_thickness) ? left : right;
    for (size_t inset_idx = 0; inset_idx < std::min(left.bead_widths.size(), right.bead_widths.size()); inset_idx++)
    {
        if (left.bead_widths[inset_idx] == 0 || right.bead_widths[inset_idx] == 0)
        {
            ret.bead_widths[inset_idx] = 0; // 0-width wall markers stay 0-width.
        }
        else
        {
            ret.bead_widths[inset_idx] = ratio_left_to_whole * left.bead_widths[inset_idx] + ratio_right_to_whole * right.bead_widths[inset_idx];
        }
        ret.toolpath_locations[inset_idx] = ratio_left_to_whole * left.toolpath_locations[inset_idx] + ratio_right_to_whole * right.toolpath_locations[inset_idx];
    }
    return ret;
}

void SkeletalTrapezoidation::generateJunctions(ptr_vector_t<BeadingPropagation>& node_beadings, ptr_vector_t<LineJunctions>& edge_junctions)
{
    for (edge_t& edge_ : graph_.edges)
    {
        edge_t* edge = &edge_;
        if (edge->from_->data_.distance_to_boundary_ > edge->to_->data_.distance_to_boundary_)
        { // Only consider the upward half-edges
            continue;
        }

        coord_t start_R = edge->to_->data_.distance_to_boundary_; // higher R
        coord_t end_R = edge->from_->data_.distance_to_boundary_; // lower R

        if ((edge->from_->data_.bead_count_ == edge->to_->data_.bead_count_ && edge->from_->data_.bead_count_ >= 0) || end_R >= start_R)
        { // No beads to generate
            continue;
        }

        auto beading_ptr = getOrCreateBeading(edge->to_, node_beadings);
        if (!CURA_CHECK_WITH_ERROR(beading_ptr != nullptr, "获取beading失败"))
        {
            CURA_ERROR_FLUSH_F("无法获取beading: edge->to_位置(%d,%d)",
                              edge->to_->p_.X, edge->to_->p_.Y);
            continue;
        }

        Beading* beading = &beading_ptr->beading_;
        edge_junctions.emplace_back(std::make_shared<LineJunctions>());
        edge_.data_.setExtrusionJunctions(edge_junctions.back()); // initialization
        LineJunctions& ret = *edge_junctions.back();

        // 使用崩溃安全检查替代assert
        coord_t expected_junction_thickness = edge->to_->data_.distance_to_boundary_ * 2;
        if (!CURA_CHECK_WITH_ERROR(beading->total_thickness >= expected_junction_thickness,
            "生成的junction厚度小于期望值"))
        {
            CURA_ERROR_FLUSH_F("Junction厚度检查失败:");
            CURA_ERROR_FLUSH_F("  - total_thickness: %d", beading->total_thickness);
            CURA_ERROR_FLUSH_F("  - expected (distance*2): %d", expected_junction_thickness);
            CURA_ERROR_FLUSH_F("  - to_distance_to_boundary: %d", edge->to_->data_.distance_to_boundary_);
            spdlog::warn("Generated junction is beyond the center of total width.");
        }

        Point2LL a = edge->to_->p_;
        Point2LL b = edge->from_->p_;
        Point2LL ab = b - a;

        const size_t num_junctions = beading->toolpath_locations.size();
        size_t junction_idx;
        // Compute starting junction_idx for this segment
        for (junction_idx = (std::max(size_t(1), beading->toolpath_locations.size()) - 1) / 2; junction_idx < num_junctions; junction_idx--)
        {
            coord_t bead_R = beading->toolpath_locations[junction_idx];
            // Adding a small epsilon (+1) to resolve an edge-case caused by rounding errors. (Would result in missing middle line.)
            if (bead_R <= start_R + 1)
            { // Junction coinciding with start node is used in this function call
                break;
            }
        }

        // Robustness against odd segments which might lie just slightly outside of the range due to rounding errors
        // not sure if this is really needed (TODO)
        if (junction_idx + 1 < num_junctions && beading->toolpath_locations[junction_idx + 1] <= start_R + 5 && beading->total_thickness < start_R + 5)
        {
            junction_idx++;
        }

        for (; junction_idx < num_junctions; junction_idx--) // When junction_idx underflows, it'll be more than num_junctions too.
        {
            coord_t bead_R = beading->toolpath_locations[junction_idx];
            assert(bead_R >= 0);
            if (bead_R < end_R)
            { // Junction coinciding with a node is handled by the next segment
                break;
            }
            Point2LL junction(a + ab * (bead_R - start_R) / (end_R - start_R));
            if (bead_R > start_R - 5)
            { // Snap to start node if it is really close, in order to be able to see 3-way intersection later on more robustly
                junction = a;
            }
            ret.emplace_back(junction, beading->bead_widths[junction_idx], junction_idx);
        }
    }
}

std::shared_ptr<SkeletalTrapezoidationJoint::BeadingPropagation> SkeletalTrapezoidation::getOrCreateBeading(node_t* node, ptr_vector_t<BeadingPropagation>& node_beadings)
{
    // 进一步减少调试输出，避免多线程混乱
    static thread_local size_t call_count = 0;
    call_count++;
    if (call_count <= 1 || call_count % 10000 == 0)  // 只记录第1次和每10000次
    {
        CURA_ERROR_FLUSH_F("=== getOrCreateBeading #%zu ===", call_count);
    }

    // 验证输入参数
    if (!CURA_CHECK_WITH_ERROR(node != nullptr, "getOrCreateBeading中node为空指针"))
    {
        CURA_ERROR_FLUSH("node为空，无法获取或创建beading");
        return nullptr;
    }

    // 减少频繁的节点处理日志
    static thread_local size_t node_count = 0;
    node_count++;
    if (node_count <= 2 || node_count % 10000 == 0)
    {
        CURA_ERROR_FLUSH_F("处理节点: 位置(%d,%d), bead_count=%d, distance_to_boundary=%d",
                          node->p_.X, node->p_.Y, node->data_.bead_count_, node->data_.distance_to_boundary_);
    }

    if (! node->data_.hasBeading())
    {
        CURA_ERROR_FLUSH("节点没有beading，需要创建");
        if (node->data_.bead_count_ == -1)
        { // This bug is due to too small central edges
            constexpr coord_t nearby_dist = 100; // TODO
            auto nearest_beading = getNearestBeading(node, nearby_dist);
            if (nearest_beading)
            {
                return nearest_beading;
            }

            // Else make a new beading:
            CURA_ERROR_FLUSH("创建新的beading");

            // 验证节点的关联边缘
            if (!CURA_CHECK_WITH_ERROR(node->incident_edge_ != nullptr, "节点没有关联边缘"))
            {
                CURA_ERROR_FLUSH_F("节点位置: (%d,%d)", node->p_.X, node->p_.Y);
                // 创建一个默认的beading
                node_beadings.emplace_back(new BeadingPropagation(beading_strategy_.compute(100, 1)));
                node->data_.setBeading(node_beadings.back());
                return node->data_.getBeading();
            }

            bool has_central_edge = false;
            bool first = true;
            coord_t dist = std::numeric_limits<coord_t>::max();
            size_t edge_count = 0;

            for (edge_t* edge = node->incident_edge_; edge && (first || edge != node->incident_edge_); edge = edge->twin_->next_)
            {
                edge_count++;

                // 防止无限循环
                if (edge_count > 100)
                {
                    CURA_ERROR_FLUSH_F("边缘遍历超过100个，可能存在循环，停止遍历");
                    break;
                }

                // 验证边缘有效性
                if (!CURA_CHECK_WITH_ERROR(edge->to_ != nullptr && edge->from_ != nullptr, "边缘节点为空"))
                {
                    CURA_ERROR_FLUSH_F("无效边缘: edge=%p", static_cast<void*>(edge));
                    continue;
                }

                // 验证twin和next指针
                if (!CURA_CHECK_WITH_ERROR(edge->twin_ != nullptr, "边缘缺少twin指针"))
                {
                    CURA_ERROR_FLUSH_F("边缘缺少twin: edge=%p", static_cast<void*>(edge));
                    break; // 无法继续遍历
                }

                if (!CURA_CHECK_WITH_ERROR(edge->twin_->next_ != nullptr, "twin边缘缺少next指针"))
                {
                    CURA_ERROR_FLUSH_F("twin边缘缺少next: twin=%p", static_cast<void*>(edge->twin_));
                    break; // 无法继续遍历
                }
                if (edge->data_.isCentral())
                {
                    has_central_edge = true;
                }
                // 使用崩溃安全检查距离边界值
                if (!CURA_CHECK_WITH_ERROR(edge->to_->data_.distance_to_boundary_ >= 0, "节点到边界距离为负数"))
                {
                    CURA_ERROR_FLUSH_F("负距离边界值:");
                    CURA_ERROR_FLUSH_F("  - edge->to_位置: (%d,%d)", edge->to_->p_.X, edge->to_->p_.Y);
                    CURA_ERROR_FLUSH_F("  - distance_to_boundary: %d", edge->to_->data_.distance_to_boundary_);
                    CURA_ERROR_FLUSH_F("  - edge长度: %d", vSize(edge->to_->p_ - edge->from_->p_));
                    CURA_ERROR_FLUSH("  - 可能原因: 距离计算错误或初始化失败");
                    // 修正为0
                    edge->to_->data_.distance_to_boundary_ = 0;
                }

                coord_t edge_length = vSize(edge->to_->p_ - edge->from_->p_);
                coord_t candidate_dist = edge->to_->data_.distance_to_boundary_ + edge_length;
                dist = std::min(dist, candidate_dist);
                first = false;
            }

            if (! has_central_edge)
            {
                CURA_ERROR_FLUSH_F("非中心节点的未知beading: node位置(%d,%d)", node->p_.X, node->p_.Y);
            }

            // 使用崩溃安全检查距离计算结果
            if (!CURA_CHECK_WITH_ERROR(dist != std::numeric_limits<coord_t>::max(), "无法计算节点到边界的距离"))
            {
                CURA_ERROR_FLUSH_F("距离计算失败:");
                CURA_ERROR_FLUSH_F("  - node位置: (%d,%d)", node->p_.X, node->p_.Y);
                CURA_ERROR_FLUSH_F("  - has_central_edge: %s", has_central_edge ? "true" : "false");
                CURA_ERROR_FLUSH_F("  - 检查的边缘数量: %d", first ? 0 : 1);
                CURA_ERROR_FLUSH("  - 可能原因: 没有中心边缘或距离计算失败");
                // 设置一个默认距离避免崩溃
                dist = 100; // 设置一个合理的默认值
                CURA_ERROR_FLUSH_F("  - 已设置默认距离: %d", dist);
            }
            coord_t bead_input = dist * 2;
            node->data_.bead_count_ = beading_strategy_.getOptimalBeadCount(bead_input);

            CURA_ERROR_FLUSH_F("节点bead计算: 位置(%d,%d), dist=%d, input=%d, bead_count=%d",
                              node->p_.X, node->p_.Y, dist, bead_input, node->data_.bead_count_);
        }

        // 使用崩溃安全检查bead计数有效性
        if (!CURA_CHECK_WITH_ERROR(node->data_.bead_count_ != -1, "节点bead计数未正确设置"))
        {
            CURA_ERROR_FLUSH_F("Bead计数无效:");
            CURA_ERROR_FLUSH_F("  - node位置: (%d,%d)", node->p_.X, node->p_.Y);
            CURA_ERROR_FLUSH_F("  - bead_count: %d", node->data_.bead_count_);
            CURA_ERROR_FLUSH_F("  - distance_to_boundary: %d", node->data_.distance_to_boundary_);
            CURA_ERROR_FLUSH_F("  - 输入到getOptimalBeadCount: %d", node->data_.distance_to_boundary_ * 2);
            CURA_ERROR_FLUSH("  - 可能原因: BeadingStrategy计算失败或输入超出范围");
            // 设置一个默认的bead计数
            node->data_.bead_count_ = 0;
            CURA_ERROR_FLUSH_F("  - 已设置默认bead_count: %d", node->data_.bead_count_);
        }

        // 尝试计算beading数据
        try {
            coord_t beading_input = node->data_.distance_to_boundary_ * 2;
            auto beading = beading_strategy_.compute(beading_input, node->data_.bead_count_);
            node_beadings.emplace_back(new BeadingPropagation(beading));
            node->data_.setBeading(node_beadings.back());

            CURA_ERROR_FLUSH_F("Beading计算成功: input=%d, bead_count=%d",
                              beading_input, node->data_.bead_count_);
        } catch (const std::exception& e) {
            CURA_ERROR_FLUSH_F("Beading计算异常: %s", e.what());
            // 使用默认值
            node_beadings.emplace_back(new BeadingPropagation(beading_strategy_.compute(100, 1)));
            node->data_.setBeading(node_beadings.back());
        }
    }

    // 使用崩溃安全检查beading数据存在性
    if (!CURA_CHECK_WITH_ERROR(node->data_.hasBeading(), "节点没有beading数据"))
    {
        CURA_ERROR_FLUSH_F("Beading数据缺失:");
        CURA_ERROR_FLUSH_F("  - node位置: (%d,%d)", node->p_.X, node->p_.Y);
        CURA_ERROR_FLUSH_F("  - bead_count: %d", node->data_.bead_count_);
        CURA_ERROR_FLUSH_F("  - distance_to_boundary: %d", node->data_.distance_to_boundary_);
        CURA_ERROR_FLUSH_F("  - node_beadings.size(): %zu", node_beadings.size());
        CURA_ERROR_FLUSH("  - 可能原因: beading计算或设置失败");
        // 创建一个默认的beading避免崩溃
        node_beadings.emplace_back(new BeadingPropagation(beading_strategy_.compute(100, 1))); // 默认值
        node->data_.setBeading(node_beadings.back());
        CURA_ERROR_FLUSH("  - 已创建默认beading数据");
    }
    return node->data_.getBeading();
}

std::shared_ptr<SkeletalTrapezoidationJoint::BeadingPropagation> SkeletalTrapezoidation::getNearestBeading(node_t* node, coord_t max_dist)
{
    // 进一步减少调试输出，避免多线程混乱
    static thread_local size_t call_count = 0;
    call_count++;
    if (call_count <= 1 || call_count % 10000 == 0)  // 只记录第1次和每10000次
    {
        CURA_ERROR_FLUSH_F("=== getNearestBeading #%zu ===", call_count);
    }

    // 验证输入参数
    if (!CURA_CHECK_WITH_ERROR(node != nullptr, "getNearestBeading中node为空指针"))
    {
        CURA_ERROR_FLUSH("node为空，无法查找最近beading");
        return nullptr;
    }

    if (!CURA_CHECK_WITH_ERROR(node->incident_edge_ != nullptr, "节点没有关联边缘"))
    {
        CURA_ERROR_FLUSH_F("节点位置: (%d,%d)", node->p_.X, node->p_.Y);
        return nullptr;
    }

    CURA_ERROR_FLUSH_F("查找节点(%d,%d)的最近beading，最大距离: %d",
                      node->p_.X, node->p_.Y, max_dist);

    struct DistEdge
    {
        edge_t* edge_to_;
        coord_t dist_;

        DistEdge(edge_t* edge_to, coord_t dist)
            : edge_to_(edge_to)
            , dist_(dist)
        {
        }
    };

    auto compare = [](const DistEdge& l, const DistEdge& r) -> bool
    {
        return l.dist_ > r.dist_;
    };
    std::priority_queue<DistEdge, std::vector<DistEdge>, decltype(compare)> further_edges(compare);
    bool first = true;
    size_t edge_count = 0;

    for (edge_t* outgoing = node->incident_edge_; outgoing && (first || outgoing != node->incident_edge_); outgoing = outgoing->twin_->next_)
    {
        edge_count++;

        // 防止无限循环
        if (edge_count > 100)
        {
            CURA_ERROR_FLUSH_F("边缘遍历超过100个，可能存在循环，停止遍历");
            break;
        }

        // 验证边缘有效性
        if (!CURA_CHECK_WITH_ERROR(outgoing->to_ != nullptr, "边缘的to_节点为空"))
        {
            CURA_ERROR_FLUSH_F("无效边缘: outgoing=%p", static_cast<void*>(outgoing));
            continue;
        }

        if (!CURA_CHECK_WITH_ERROR(outgoing->from_ != nullptr, "边缘的from_节点为空"))
        {
            CURA_ERROR_FLUSH_F("无效边缘: outgoing=%p", static_cast<void*>(outgoing));
            continue;
        }

        // 验证twin和next指针
        if (!CURA_CHECK_WITH_ERROR(outgoing->twin_ != nullptr, "边缘缺少twin指针"))
        {
            CURA_ERROR_FLUSH_F("边缘缺少twin: outgoing=%p", static_cast<void*>(outgoing));
            break; // 无法继续遍历
        }

        if (!CURA_CHECK_WITH_ERROR(outgoing->twin_->next_ != nullptr, "twin边缘缺少next指针"))
        {
            CURA_ERROR_FLUSH_F("twin边缘缺少next: twin=%p", static_cast<void*>(outgoing->twin_));
            break; // 无法继续遍历
        }

        coord_t edge_length = vSize(outgoing->to_->p_ - outgoing->from_->p_);
        further_edges.emplace(outgoing, edge_length);
        first = false;
    }

    CURA_ERROR_FLUSH_F("初始化完成，找到%zu个边缘", edge_count);

    for (coord_t counter = 0; counter < SKELETAL_TRAPEZOIDATION_BEAD_SEARCH_MAX; counter++)
    { // Prevent endless recursion
        if (further_edges.empty())
        {
            CURA_ERROR_FLUSH_F("搜索队列为空，未找到beading (迭代%d)", counter);
            return nullptr;
        }

        DistEdge here = further_edges.top();
        further_edges.pop();

        if (here.dist_ > max_dist)
        {
            CURA_ERROR_FLUSH_F("距离超过最大值: %d > %d (迭代%d)", here.dist_, max_dist, counter);
            return nullptr;
        }

        // 验证当前边缘有效性
        if (!CURA_CHECK_WITH_ERROR(here.edge_to_ != nullptr, "搜索中的边缘为空"))
        {
            CURA_ERROR_FLUSH_F("搜索中发现空边缘 (迭代%d)", counter);
            continue;
        }

        if (!CURA_CHECK_WITH_ERROR(here.edge_to_->to_ != nullptr, "搜索中的边缘to_节点为空"))
        {
            CURA_ERROR_FLUSH_F("搜索中发现无效边缘 (迭代%d)", counter);
            continue;
        }

        if (here.edge_to_->to_->data_.hasBeading())
        {
            CURA_ERROR_FLUSH_F("找到beading，距离: %d (迭代%d)", here.dist_, counter);
            return here.edge_to_->to_->data_.getBeading();
        }
        else
        { // recurse
            CURA_ERROR_FLUSH_F("递归搜索，当前距离: %d (迭代%d)", here.dist_, counter);

            // 验证next指针
            if (!CURA_CHECK_WITH_ERROR(here.edge_to_->next_ != nullptr, "边缘缺少next指针"))
            {
                CURA_ERROR_FLUSH_F("边缘缺少next，跳过递归 (迭代%d)", counter);
                continue;
            }

            size_t recursion_count = 0;
            for (edge_t* further_edge = here.edge_to_->next_;
                 further_edge && further_edge != here.edge_to_->twin_;
                 further_edge = further_edge->twin_->next_)
            {
                recursion_count++;

                // 防止递归过深
                if (recursion_count > 50)
                {
                    CURA_ERROR_FLUSH_F("递归深度过深，停止 (迭代%d)", counter);
                    break;
                }

                // 验证递归边缘
                if (!CURA_CHECK_WITH_ERROR(further_edge->to_ != nullptr && further_edge->from_ != nullptr,
                    "递归边缘节点为空"))
                {
                    continue;
                }

                if (!CURA_CHECK_WITH_ERROR(further_edge->twin_ != nullptr && further_edge->twin_->next_ != nullptr,
                    "递归边缘缺少twin或next"))
                {
                    break;
                }

                coord_t additional_dist = vSize(further_edge->to_->p_ - further_edge->from_->p_);
                further_edges.emplace(further_edge, here.dist_ + additional_dist);
            }
        }
    }

    CURA_ERROR_FLUSH_F("达到最大搜索次数 (%d)，未找到beading", SKELETAL_TRAPEZOIDATION_BEAD_SEARCH_MAX);
    return nullptr;
}

void SkeletalTrapezoidation::addToolpathSegment(const ExtrusionJunction& from, const ExtrusionJunction& to, bool is_odd, bool force_new_path, bool from_is_3way, bool to_is_3way)
{
    if (from == to)
        return;

    std::vector<VariableWidthLines>& generated_toolpaths = *p_generated_toolpaths;

    size_t inset_idx = from.perimeter_index_;
    if (inset_idx >= generated_toolpaths.size())
    {
        generated_toolpaths.resize(inset_idx + 1);
    }
    // 使用崩溃安全检查替代assert
    if (!CURA_CHECK_WITH_ERROR(
        generated_toolpaths[inset_idx].empty() || !generated_toolpaths[inset_idx].back().junctions_.empty(),
        "生成了空的挤出线"))
    {
        CURA_ERROR_FLUSH_F("空挤出线错误:");
        CURA_ERROR_FLUSH_F("  - inset_idx: %zu", inset_idx);
        CURA_ERROR_FLUSH_F("  - generated_toolpaths大小: %zu", generated_toolpaths.size());
        if (!generated_toolpaths[inset_idx].empty()) {
            CURA_ERROR_FLUSH_F("  - back().junctions_大小: %zu",
                              generated_toolpaths[inset_idx].back().junctions_.size());
        }
        CURA_ERROR_FLUSH("  - 可能原因: 工具路径生成逻辑错误");
        // 清理空的挤出线
        if (!generated_toolpaths[inset_idx].empty() && generated_toolpaths[inset_idx].back().junctions_.empty()) {
            generated_toolpaths[inset_idx].pop_back();
        }
    }
    if (generated_toolpaths[inset_idx].empty() || generated_toolpaths[inset_idx].back().is_odd_ != is_odd
        || generated_toolpaths[inset_idx].back().junctions_.back().perimeter_index_ != inset_idx // inset_idx should always be consistent
    )
    {
        force_new_path = true;
    }
    if (! force_new_path && shorterThen(generated_toolpaths[inset_idx].back().junctions_.back().p_ - from.p_, 10)
        && std::abs(generated_toolpaths[inset_idx].back().junctions_.back().w_ - from.w_) < 10 && ! from_is_3way // force new path at 3way intersection
    )
    {
        generated_toolpaths[inset_idx].back().junctions_.push_back(to);
    }
    else if (
        ! force_new_path && shorterThen(generated_toolpaths[inset_idx].back().junctions_.back().p_ - to.p_, 10)
        && std::abs(generated_toolpaths[inset_idx].back().junctions_.back().w_ - to.w_) < 10 && ! to_is_3way // force new path at 3way intersection
    )
    {
        if (! is_odd)
        {
            spdlog::error("Reversing even wall line causes it to be printed CCW instead of CW!");
        }
        generated_toolpaths[inset_idx].back().junctions_.push_back(from);
    }
    else
    {
        generated_toolpaths[inset_idx].emplace_back(inset_idx, is_odd);
        generated_toolpaths[inset_idx].back().junctions_.push_back(from);
        generated_toolpaths[inset_idx].back().junctions_.push_back(to);
    }
};

void SkeletalTrapezoidation::connectJunctions(ptr_vector_t<LineJunctions>& edge_junctions)
{
    // 进一步减少调试输出，避免多线程混乱
    static thread_local size_t call_count = 0;
    call_count++;
    if (call_count <= 1 || call_count % 10000 == 0)  // 只记录第1次和每10000次
    {
        CURA_ERROR_FLUSH_F("=== connectJunctions #%zu ===", call_count);
        CURA_ERROR_FLUSH_F("图边缘数量: %zu", graph_.edges.size());
    }

    std::unordered_set<edge_t*> unprocessed_quad_starts(graph_.edges.size() * 5 / 2);
    for (edge_t& edge : graph_.edges)
    {
        if (! edge.prev_)
        {
            unprocessed_quad_starts.insert(&edge);
        }
    }

    if (call_count <= 3 || call_count % 1000 == 0)
    {
        CURA_ERROR_FLUSH_F("找到%zu个未处理的quad起始点", unprocessed_quad_starts.size());
    }

    std::unordered_set<edge_t*> passed_odd_edges;

    size_t main_loop_count = 0;
    while (! unprocessed_quad_starts.empty())
    {
        main_loop_count++;

        // 防止主循环无限循环
        if (main_loop_count > 100000)
        {
            CURA_ERROR_FLUSH_F("主循环超过100000次，可能存在无限循环，强制退出");
            CURA_ERROR_FLUSH_F("剩余未处理的quad: %zu", unprocessed_quad_starts.size());
            break;
        }

        edge_t* poly_domain_start = *unprocessed_quad_starts.begin();

        // 验证poly_domain_start有效性
        if (!CURA_CHECK_WITH_ERROR(poly_domain_start != nullptr, "poly_domain_start为空"))
        {
            CURA_ERROR_FLUSH("poly_domain_start为空，移除并继续");
            unprocessed_quad_starts.erase(unprocessed_quad_starts.begin());
            continue;
        }

        edge_t* quad_start = poly_domain_start;
        bool new_domain_start = true;
        size_t do_loop_count = 0;
        do
        {
            do_loop_count++;

            // 防止do-while循环无限循环
            if (do_loop_count > 10000)
            {
                CURA_ERROR_FLUSH_F("do-while循环超过10000次，可能存在无限循环，强制退出");
                break;
            }
            edge_t* quad_end = quad_start;
            size_t loop_count = 0;
            while (quad_end->next_)
            {
                // 防止无限循环
                loop_count++;
                if (loop_count > 10000)
                {
                    CURA_ERROR_FLUSH_F("quad_end循环超过10000次，可能存在无限循环");
                    CURA_ERROR_FLUSH_F("当前quad_end位置: (%d,%d)",
                                      quad_end->to_->p_.X, quad_end->to_->p_.Y);
                    break;
                }

                // 验证next指针有效性
                if (!CURA_CHECK_WITH_ERROR(quad_end->next_->to_ != nullptr &&
                    quad_end->next_->from_ != nullptr, "quad_end->next_节点为空"))
                {
                    CURA_ERROR_FLUSH_F("quad_end->next_节点为空，停止循环");
                    break;
                }

                quad_end = quad_end->next_;
            }

            // 验证quad_start有效性
            if (!CURA_CHECK_WITH_ERROR(quad_start != nullptr && quad_start->to_ != nullptr &&
                quad_start->from_ != nullptr, "quad_start无效"))
            {
                CURA_ERROR_FLUSH_F("quad_start无效，跳过此quad");
                unprocessed_quad_starts.erase(quad_start);
                break;
            }

            edge_t* edge_to_peak = getQuadMaxRedgeTo(quad_start);

            // 验证edge_to_peak有效性
            if (!CURA_CHECK_WITH_ERROR(edge_to_peak != nullptr && edge_to_peak->to_ != nullptr &&
                edge_to_peak->from_ != nullptr, "edge_to_peak无效"))
            {
                CURA_ERROR_FLUSH_F("edge_to_peak无效，跳过此quad");
                unprocessed_quad_starts.erase(quad_start);
                break;
            }

            // walk down on both sides and connect junctions
            edge_t* edge_from_peak = edge_to_peak->next_;

            // 使用崩溃安全检查替代assert
            if (!CURA_CHECK_WITH_ERROR(edge_from_peak != nullptr, "edge_from_peak为空"))
            {
                CURA_ERROR_FLUSH_F("edge_to_peak位置: (%d,%d)",
                                  edge_to_peak->to_->p_.X, edge_to_peak->to_->p_.Y);
                CURA_ERROR_FLUSH("跳过此quad，继续处理下一个");
                unprocessed_quad_starts.erase(quad_start);
                continue;
            }

            unprocessed_quad_starts.erase(quad_start);

            if (! edge_to_peak->data_.hasExtrusionJunctions())
            {
                edge_junctions.emplace_back(std::make_shared<LineJunctions>());
                edge_to_peak->data_.setExtrusionJunctions(edge_junctions.back());
            }
            // The junctions on the edge(s) from the start of the quad to the node with highest R
            LineJunctions from_junctions = *edge_to_peak->data_.getExtrusionJunctions();
            if (! edge_from_peak->twin_->data_.hasExtrusionJunctions())
            {
                edge_junctions.emplace_back(std::make_shared<LineJunctions>());
                edge_from_peak->twin_->data_.setExtrusionJunctions(edge_junctions.back());
            }
            // The junctions on the edge(s) from the end of the quad to the node with highest R
            LineJunctions to_junctions = *edge_from_peak->twin_->data_.getExtrusionJunctions();
            if (edge_to_peak->prev_)
            {
                LineJunctions from_prev_junctions = *edge_to_peak->prev_->data_.getExtrusionJunctions();
                while (! from_junctions.empty() && ! from_prev_junctions.empty() && from_junctions.back().perimeter_index_ <= from_prev_junctions.front().perimeter_index_)
                {
                    from_junctions.pop_back();
                }
                from_junctions.reserve(from_junctions.size() + from_prev_junctions.size());
                from_junctions.insert(from_junctions.end(), from_prev_junctions.begin(), from_prev_junctions.end());

                // 使用崩溃安全检查替代assert
                if (!CURA_CHECK_WITH_ERROR(edge_to_peak->prev_->prev_ == nullptr,
                    "边缘已经连接"))
                {
                    CURA_ERROR_FLUSH("边缘已经连接，可能导致重复连接");
                    spdlog::warn("The edge we're about to connect is already connected.");
                }
            }
            if (edge_from_peak->next_)
            {
                LineJunctions to_next_junctions = *edge_from_peak->next_->twin_->data_.getExtrusionJunctions();
                while (! to_junctions.empty() && ! to_next_junctions.empty() && to_junctions.back().perimeter_index_ <= to_next_junctions.front().perimeter_index_)
                {
                    to_junctions.pop_back();
                }
                to_junctions.reserve(to_junctions.size() + to_next_junctions.size());
                to_junctions.insert(to_junctions.end(), to_next_junctions.begin(), to_next_junctions.end());

                // 使用崩溃安全检查替代assert
                if (!CURA_CHECK_WITH_ERROR(edge_from_peak->next_->next_ == nullptr,
                    "边缘已经连接"))
                {
                    CURA_ERROR_FLUSH("边缘已经连接，可能导致重复连接");
                    spdlog::warn("The edge we're about to connect is already connected!");
                }
            }
            // 使用崩溃安全检查替代assert
            int size_diff = std::abs(int(from_junctions.size()) - int(to_junctions.size()));
            if (!CURA_CHECK_WITH_ERROR(size_diff <= 1, "junction数量差异过大"))
            {
                CURA_ERROR_FLUSH_F("Junction数量差异检查失败:");
                CURA_ERROR_FLUSH_F("  - from_junctions.size(): %zu", from_junctions.size());
                CURA_ERROR_FLUSH_F("  - to_junctions.size(): %zu", to_junctions.size());
                CURA_ERROR_FLUSH_F("  - 差异: %d", size_diff);
                spdlog::warn(
                    "Can't create a transition when connecting two perimeters where the number of beads differs too much! {} vs. {}",
                    from_junctions.size(),
                    to_junctions.size());
                // 继续处理，但跳过这个连接
                unprocessed_quad_starts.erase(quad_start);
                continue;
            }

            size_t segment_count = std::min(from_junctions.size(), to_junctions.size());
            for (size_t junction_rev_idx = 0; junction_rev_idx < segment_count; junction_rev_idx++)
            {
                // 验证数组边界
                if (!CURA_CHECK_WITH_ERROR(junction_rev_idx < from_junctions.size() &&
                    junction_rev_idx < to_junctions.size(), "junction索引越界"))
                {
                    CURA_ERROR_FLUSH_F("Junction索引越界:");
                    CURA_ERROR_FLUSH_F("  - junction_rev_idx: %zu", junction_rev_idx);
                    CURA_ERROR_FLUSH_F("  - from_junctions.size(): %zu", from_junctions.size());
                    CURA_ERROR_FLUSH_F("  - to_junctions.size(): %zu", to_junctions.size());
                    break; // 跳出循环，避免崩溃
                }

                ExtrusionJunction& from = from_junctions[from_junctions.size() - 1 - junction_rev_idx];
                ExtrusionJunction& to = to_junctions[to_junctions.size() - 1 - junction_rev_idx];

                // 使用崩溃安全检查替代assert
                if (!CURA_CHECK_WITH_ERROR(from.perimeter_index_ == to.perimeter_index_,
                    "连接的perimeter索引不匹配"))
                {
                    CURA_ERROR_FLUSH_F("Perimeter索引不匹配:");
                    CURA_ERROR_FLUSH_F("  - from.perimeter_index_: %d", from.perimeter_index_);
                    CURA_ERROR_FLUSH_F("  - to.perimeter_index_: %d", to.perimeter_index_);
                    spdlog::warn("Connecting two perimeters with different indices! Perimeter {} and {}",
                                from.perimeter_index_, to.perimeter_index_);
                    // 继续处理，但记录警告
                }
                const bool from_is_odd = quad_start->to_->data_.bead_count_ > 0 && quad_start->to_->data_.bead_count_ % 2 == 1 // quad contains single bead segment
                                      && quad_start->to_->data_.transition_ratio_ == 0 // We're not in a transition
                                      && junction_rev_idx == segment_count - 1 // Is single bead segment
                                      && shorterThen(from.p_ - quad_start->to_->p_, 5);
                const bool to_is_odd = quad_end->from_->data_.bead_count_ > 0 && quad_end->from_->data_.bead_count_ % 2 == 1 // quad contains single bead segment
                                    && quad_end->from_->data_.transition_ratio_ == 0 // We're not in a transition
                                    && junction_rev_idx == segment_count - 1 // Is single bead segment
                                    && shorterThen(to.p_ - quad_end->from_->p_, 5);
                const bool is_odd_segment = from_is_odd && to_is_odd;

                if (is_odd_segment && passed_odd_edges.count(quad_start->next_->twin_) > 0) // Only generate toolpath for odd segments once
                {
                    continue; // Prevent duplication of single bead segments
                }

                bool from_is_3way = from_is_odd && quad_start->to_->isMultiIntersection();
                bool to_is_3way = to_is_odd && quad_end->from_->isMultiIntersection();

                passed_odd_edges.emplace(quad_start->next_);

                addToolpathSegment(from, to, is_odd_segment, new_domain_start, from_is_3way, to_is_3way);
            }
            new_domain_start = false;
        } while (quad_start = quad_start->getNextUnconnected(), quad_start != poly_domain_start);
    }
}

void SkeletalTrapezoidation::generateLocalMaximaSingleBeads()
{
    std::vector<VariableWidthLines>& generated_toolpaths = *p_generated_toolpaths;

    const auto addCircleToToolpath = [&](const Point2LL& center, coord_t width, size_t inset_index)
    {
        if (inset_index >= generated_toolpaths.size())
        {
            generated_toolpaths.resize(inset_index + 1);
        }
        constexpr bool is_odd = true;
        generated_toolpaths[inset_index].emplace_back(inset_index, is_odd);
        ExtrusionLine& line = generated_toolpaths[inset_index].back();
        // total area to be extruded is pi*(w/2)^2 = pi*w*w/4
        // Width a constant extrusion width w, that would be a length of pi*w/4
        // If we make a small circle to fill up the hole, then that circle would have a circumference of 2*pi*r
        // So our circle needs to be such that r=w/8
        const coord_t r = width / 8;
        constexpr coord_t n_segments = 6;
        const auto circle = PolygonUtils::makeCircle<std::vector<ExtrusionJunction>, true>(center, r, n_segments, width, inset_index);
        line.junctions_.insert(line.junctions_.end(), circle.begin(), circle.end());
    };

    Point2LL local_maxima_accumulator;
    coord_t width_accumulator = 0;
    size_t accumulator_count = 0;

    for (const auto& node : graph_.nodes)
    {
        if (! node.data_.hasBeading())
        {
            continue;
        }
        const Beading& beading = node.data_.getBeading()->beading_;
        if (beading.bead_widths.size() % 2 == 1 && node.isLocalMaximum(true))
        {
            const size_t inset_index = beading.bead_widths.size() / 2;
            const coord_t width = beading.bead_widths[inset_index];
            local_maxima_accumulator += node.p_;
            width_accumulator += width;
            ++accumulator_count;
            if (! node.isCentral())
            {
                addCircleToToolpath(node.p_, width, inset_index);
            }
        }
    }

    if (accumulator_count > 0)
    {
        bool replace_with_local_maxima = generated_toolpaths.empty() || generated_toolpaths[0].empty();
        coord_t total_path_length = 0;
        if (! replace_with_local_maxima)
        {
            coord_t min_width = std::numeric_limits<coord_t>::max();
            for (const auto& line : generated_toolpaths[0])
            {
                total_path_length += line.length();
                for (const ExtrusionJunction& j : line)
                {
                    min_width = std::min(min_width, j.w_);
                }
            }
            replace_with_local_maxima |= total_path_length <= min_width / 2;
        }
        if (replace_with_local_maxima)
        {
            const coord_t width = width_accumulator / accumulator_count;
            local_maxima_accumulator = local_maxima_accumulator / accumulator_count;
            if (generated_toolpaths.empty())
            {
                generated_toolpaths.emplace_back();
            }
            else
            {
                generated_toolpaths[0].clear();
            }
            addCircleToToolpath(local_maxima_accumulator, width, 0);
        }
    }
}
//
// ^^^^^^^^^^^^^^^^^^^^^
//  TOOLPATH GENERATION
// =====================
//

void SkeletalTrapezoidation::validateAndFixGraphIntegrity()
{
    CURA_ERROR_FLUSH("=== 开始图完整性验证和修复 ===");

    size_t total_edges = graph_.edges.size();
    size_t edges_without_twin = 0;
    size_t edges_fixed = 0;

    // 第一遍：统计和标记没有twin的边缘
    std::vector<edge_t*> edges_without_twin_list;

    for (edge_t& edge : graph_.edges)
    {
        if (!edge.twin_)
        {
            edges_without_twin++;
            edges_without_twin_list.push_back(&edge);
        }
    }

    CURA_ERROR_FLUSH_F("图完整性统计: 总边缘=%zu, 缺少twin=%zu", total_edges, edges_without_twin);

    // 第二遍：尝试修复没有twin的边缘
    for (edge_t* edge : edges_without_twin_list)
    {
        // 查找可能的twin边缘（from和to相反的边缘）
        edge_t* potential_twin = nullptr;

        for (edge_t& candidate : graph_.edges)
        {
            if (&candidate != edge &&
                candidate.from_ == edge->to_ &&
                candidate.to_ == edge->from_ &&
                !candidate.twin_)
            {
                potential_twin = &candidate;
                break;
            }
        }

        if (potential_twin)
        {
            // 建立twin关系
            edge->twin_ = potential_twin;
            potential_twin->twin_ = edge;
            edges_fixed++;

            CURA_ERROR_FLUSH_F("修复twin关系: edge(%d,%d)->(%d,%d) <-> edge(%d,%d)->(%d,%d)",
                              edge->from_->p_.X, edge->from_->p_.Y, edge->to_->p_.X, edge->to_->p_.Y,
                              potential_twin->from_->p_.X, potential_twin->from_->p_.Y,
                              potential_twin->to_->p_.X, potential_twin->to_->p_.Y);
        }
        else
        {
            // 无法找到twin，创建一个虚拟的twin边缘
            graph_.edges.emplace_back(SkeletalTrapezoidationEdge());
            edge_t* virtual_twin = &graph_.edges.back();

            virtual_twin->from_ = edge->to_;
            virtual_twin->to_ = edge->from_;
            virtual_twin->twin_ = edge;
            edge->twin_ = virtual_twin;

            // 设置虚拟twin的数据
            virtual_twin->data_.setIsCentral(false);
            virtual_twin->data_.type_ = SkeletalTrapezoidationEdge::EdgeType::EXTRA_VD;

            edges_fixed++;

            CURA_ERROR_FLUSH_F("创建虚拟twin: edge(%d,%d)->(%d,%d) <-> virtual_twin(%d,%d)->(%d,%d)",
                              edge->from_->p_.X, edge->from_->p_.Y, edge->to_->p_.X, edge->to_->p_.Y,
                              virtual_twin->from_->p_.X, virtual_twin->from_->p_.Y,
                              virtual_twin->to_->p_.X, virtual_twin->to_->p_.Y);
        }
    }

    CURA_ERROR_FLUSH_F("图完整性修复完成: 修复了%zu个边缘的twin关系", edges_fixed);

    // 第三遍：验证修复结果
    size_t remaining_without_twin = 0;
    for (edge_t& edge : graph_.edges)
    {
        if (!edge.twin_)
        {
            remaining_without_twin++;
        }
    }

    CURA_ERROR_FLUSH_F("修复后统计: 总边缘=%zu, 仍缺少twin=%zu", graph_.edges.size(), remaining_without_twin);

    if (remaining_without_twin == 0)
    {
        CURA_ERROR_FLUSH("✅ 图完整性验证通过，所有边缘都有twin指针");
    }
    else
    {
        CURA_ERROR_FLUSH_F("⚠️ 仍有%zu个边缘缺少twin指针", remaining_without_twin);
    }
}

} // namespace cura
