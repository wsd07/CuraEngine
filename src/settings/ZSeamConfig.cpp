// Copyright (c) 2022 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#include "settings/ZSeamConfig.h" //The definitions we're implementing.
#include <algorithm>
#include <optional>
#include <spdlog/spdlog.h>
#include "utils/math.h" // For INT2MM
#include "utils/DebugManager.h"

namespace cura
{

ZSeamConfig::ZSeamConfig(const EZSeamType type, const Point2LL pos, const EZSeamCornerPrefType corner_pref, const coord_t simplify_curvature,
                         const bool draw_z_seam_enable, const std::vector<Point3LL>& draw_z_seam_points,
                         const bool z_seam_point_interpolation, const bool draw_z_seam_grow, const coord_t current_layer_z)
    : type_(type)
    , pos_(pos)
    , corner_pref_(corner_pref)
    , simplify_curvature_(simplify_curvature)
    , draw_z_seam_enable_(draw_z_seam_enable)
    , draw_z_seam_points_(draw_z_seam_points)
    , z_seam_point_interpolation_(z_seam_point_interpolation)
    , draw_z_seam_grow_(draw_z_seam_grow)
    , current_layer_z_(current_layer_z)
{
    // 添加调试日志（使用debug级别避免过多输出）
    if (draw_z_seam_enable_)
    {
        CURA_DEBUG(SEAM_PLACEMENT, "ZSeamConfig创建: 自定义Z接缝点功能已启用");
        CURA_DEBUG(SEAM_PLACEMENT, "接缝点数量: {}", draw_z_seam_points_.size());
        CURA_DEBUG(SEAM_PLACEMENT, "插值模式: {}", z_seam_point_interpolation_ ? "启用" : "禁用");
        CURA_DEBUG(SEAM_PLACEMENT, "超出范围处理: {}", draw_z_seam_grow_ ? "使用默认" : "使用边界点");
        CURA_DEBUG(SEAM_PLACEMENT, "当前层Z坐标: {:.2f}mm", INT2MM(current_layer_z_));

        for (size_t i = 0; i < draw_z_seam_points_.size(); ++i)
        {
            const auto& point = draw_z_seam_points_[i];
            CURA_DEBUG(SEAM_PLACEMENT, "接缝点[{}]: ({:.2f}, {:.2f}, {:.2f})",
                        i, INT2MM(point.x_), INT2MM(point.y_), INT2MM(point.z_));
        }
    }
}

std::optional<Point2LL> ZSeamConfig::getInterpolatedSeamPosition() const
{
    // 检查前置条件：功能是否启用且有有效的接缝点数据
    if (!draw_z_seam_enable_ || draw_z_seam_points_.empty())
    {
        CURA_DEBUG(SEAM_PLACEMENT, "自定义Z接缝点未启用或点列表为空，启用状态: {}, 点数量: {}",
                     draw_z_seam_enable_, draw_z_seam_points_.size());
        return std::nullopt;  // 返回空值，表示不使用自定义接缝点
    }

    // 获取当前层的Z坐标（微米单位）
    const coord_t layer_z = current_layer_z_;
    //CURA_INFO("=== 外轮廓Z接缝点插值计算 ===");
    //CURA_INFO("当前层Z坐标: {:.2f}mm", INT2MM(layer_z));

    // 特殊情况：如果只有一个接缝点，直接返回该点的XY坐标
    if (draw_z_seam_points_.size() == 1)
    {
        Point2LL result(draw_z_seam_points_[0].x_, draw_z_seam_points_[0].y_);
        CURA_DEBUG(SEAM_PLACEMENT, "只有一个接缝点，返回: ({:.2f}, {:.2f})", INT2MM(result.X), INT2MM(result.Y));
        return result;
    }

    // 创建按Z坐标排序的点列表副本（避免修改原始数据）
    // 排序确保我们可以正确进行线性插值
    std::vector<Point3LL> sorted_points = draw_z_seam_points_;
    std::sort(sorted_points.begin(), sorted_points.end(),
              [](const Point3LL& a, const Point3LL& b) { return a.z_ < b.z_; });

    // 获取Z坐标的有效范围
    const coord_t min_z = sorted_points.front().z_;  // 最低点的Z坐标
    const coord_t max_z = sorted_points.back().z_;   // 最高点的Z坐标

    // 输出调试信息：显示用户设置的Z范围
    //CURA_INFO("用户设置Z范围: {:.2f}mm - {:.2f}mm", INT2MM(min_z), INT2MM(max_z));

    // 处理边界情况1：当前层Z坐标低于最低接缝点
    if (layer_z < min_z)
    {
        // 总是使用最低点的XY坐标，确保有一个合理的接缝位置
        Point2LL result(sorted_points.front().x_, sorted_points.front().y_);
        //CURA_INFO("层Z({:.2f}mm)低于最低点({:.2f}mm)，使用最低点坐标: ({:.2f}, {:.2f})",
        //             INT2MM(layer_z), INT2MM(min_z), INT2MM(result.X), INT2MM(result.Y));
        return result;
    }
    // 处理边界情况2：当前层Z坐标高于最高接缝点
    else if (layer_z > max_z)
    {
        if (draw_z_seam_grow_)
        {
            // grow=true：超出范围时回退到默认接缝处理方式
            // 这允许系统使用其他接缝策略（如shortest、user_specified等）
            //CURA_INFO("层Z({:.2f}mm)高于最高点({:.2f}mm)且grow=true，使用默认接缝处理",
            //            INT2MM(layer_z), INT2MM(max_z));
            return std::nullopt;  // 返回空值，让系统使用默认策略
        }
        else
        {
            // grow=false：使用最高点的XY坐标，保持接缝位置固定
            Point2LL result(sorted_points.back().x_, sorted_points.back().y_);
            //CURA_INFO("层Z({:.2f}mm)高于最高点({:.2f}mm)且grow=false，使用最高点坐标: ({:.2f}, {:.2f})",
            //            INT2MM(layer_z), INT2MM(max_z), INT2MM(result.X), INT2MM(result.Y));
            return result;
        }
    }

    // 核心功能：在有效范围内进行线性插值
    // 遍历相邻的接缝点对，找到当前层Z坐标所在的区间
    for (size_t i = 0; i < sorted_points.size() - 1; ++i)
    {
        const Point3LL& p1 = sorted_points[i];      // 区间下端点
        const Point3LL& p2 = sorted_points[i + 1];  // 区间上端点

        // 检查当前层Z坐标是否在这个区间内
        if (layer_z >= p1.z_ && layer_z <= p2.z_)
        {
            // 特殊情况：正好在端点上，直接返回端点坐标
            if (layer_z == p1.z_)
            {
                return Point2LL(p1.x_, p1.y_);
            }
            if (layer_z == p2.z_)
            {
                return Point2LL(p2.x_, p2.y_);
            }

            // 线性插值计算：根据Z坐标的比例计算XY坐标
            // t = (当前Z - 起始Z) / (结束Z - 起始Z)，范围[0,1]
            const double t = static_cast<double>(layer_z - p1.z_) / static_cast<double>(p2.z_ - p1.z_);

            // 根据插值参数t计算XY坐标
            // result = p1 + t * (p2 - p1)
            const coord_t interpolated_x = p1.x_ + static_cast<coord_t>(t * (p2.x_ - p1.x_));
            const coord_t interpolated_y = p1.y_ + static_cast<coord_t>(t * (p2.y_ - p1.y_));

            Point2LL result(interpolated_x, interpolated_y);

            // 输出详细的插值信息用于调试
            //CURA_INFO("插值计算成功: 在点({:.2f},{:.2f},{:.2f})和({:.2f},{:.2f},{:.2f})之间",
            //            INT2MM(p1.x_), INT2MM(p1.y_), INT2MM(p1.z_),
            //            INT2MM(p2.x_), INT2MM(p2.y_), INT2MM(p2.z_));
            //CURA_INFO("插值参数t={:.3f}, 结果坐标: ({:.2f}, {:.2f})",
            //             t, INT2MM(result.X), INT2MM(result.Y));
            return result;
        }
    }

    // 理论上不应该到达这里
    return std::nullopt;
}

} // namespace cura
