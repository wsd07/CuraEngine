// Copyright (c) 2020 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef ZSEAMCONFIG_H
#define ZSEAMCONFIG_H

#include "EnumSettings.h" //For EZSeamType and EZSeamCornerPrefType.
#include "geometry/Point2LL.h" //To store the preferred seam position.
#include "geometry/Point3LL.h" //To store 3D seam points.
#include <vector>

namespace cura
{

/*!
 * Helper class that encapsulates the various criteria that define the location
 * of the z-seam.
 * Instances of this are passed to the PathOrderOptimizer to specify where the
 * seam is to be located.
 */
struct ZSeamConfig
{
    /*!
     * Strategy to place the seam (user-specified, shortest distance, sharpest
     * corner, etc.).
     */
    EZSeamType type_;

    /*!
     * When using a user-specified position for the seam, this is the position
     * that the user specified.
     */
    Point2LL pos_;

    /*!
     * Corner preference type, if using the sharpest corner strategy.
     */
    EZSeamCornerPrefType corner_pref_;

    /*!
     * Prevent 'smoothed out' corners (corners that are spread over multiple, very close together vertices),
     * by simplifying the polygon that the corners are detected on by this ammount.
     * This does _not_ influence the path, the simplified polygon is a temporary constructed within the algorithm.
     */
    coord_t simplify_curvature_;

    /*!
     * 自定义Z接缝点功能的相关参数
     */

    /*!
     * 是否启用自定义Z接缝点功能
     * 当启用时，系统会根据draw_z_seam_points中的3D点进行接缝位置插值
     */
    bool draw_z_seam_enable_;

    /*!
     * 用户定义的3D接缝点列表，格式为[x,y,z]坐标
     * 用于在不同高度指定接缝位置，系统会根据当前层高度进行线性插值
     * 格式示例：[x1,y1,z1],[x2,y2,z2]，坐标单位为毫米
     * 系统会自动按Z坐标排序，并在相邻两点间进行插值
     */
    std::vector<Point3LL> draw_z_seam_points_;

    /*!
     * 是否在多边形线段上进行插值查找最近点（而不是仅查找顶点）
     * true: 在多边形边上插值找到精确位置（暂未实现）
     * false: 只在现有顶点中查找最接近的点
     */
    bool z_seam_point_interpolation_;

    /*!
     * 当当前层Z坐标超出draw_z_seam_points范围时的处理方式
     * true: 使用默认接缝处理方式（如shortest、user_specified等）
     * false: 使用边界点坐标（最高点或最低点的XY坐标）
     */
    bool draw_z_seam_grow_;

    /*!
     * 当前层的Z坐标（微米单位），用于插值计算
     * 这个值在创建ZSeamConfig时传入，代表当前正在处理的层的净高度
     * （不包括raft等附加结构的高度）
     */
    coord_t current_layer_z_;

    /*!
     * Create a seam configuration with a custom configuration.
     * \param type The strategy to place the seam.
     * \param pos The position of a user-specified seam.
     * \param corner_pref The corner preference, when using the sharpest corner strategy.
     * \param simplify_curvature by how much to simplify the curvature (when detecting corners), as otherwise 'smooth' corners are penalized.
     * \param draw_z_seam_enable Whether to enable custom Z seam points.
     * \param draw_z_seam_points List of 3D seam points.
     * \param z_seam_point_interpolation Whether to interpolate on polygon segments.
     * \param draw_z_seam_grow How to handle out-of-range Z values.
     * \param current_layer_z Current layer Z coordinate for interpolation.
     */
    ZSeamConfig(
        const EZSeamType type = EZSeamType::SHORTEST,
        const Point2LL pos = Point2LL(0, 0),
        const EZSeamCornerPrefType corner_pref = EZSeamCornerPrefType::Z_SEAM_CORNER_PREF_NONE,
        const coord_t simplify_curvature = 0,
        const bool draw_z_seam_enable = false,
        const std::vector<Point3LL>& draw_z_seam_points = {},
        const bool z_seam_point_interpolation = false,
        const bool draw_z_seam_grow = true,
        const coord_t current_layer_z = 0);

    /*!
     * 根据当前层高度，从draw_z_seam_points中插值计算该层的目标接缝位置
     * 使用存储在current_layer_z_中的当前层Z坐标
     * \return 插值得到的2D坐标，如果无法插值则返回std::nullopt
     */
    std::optional<Point2LL> getInterpolatedSeamPosition() const;
};

} // namespace cura

#endif // ZSEAMCONFIG_H
