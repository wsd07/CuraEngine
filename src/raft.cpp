// Copyright (c) 2022 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#include "raft.h"

#include <polyclipping/clipper.hpp>
#include <fstream>

#include <spdlog/spdlog.h>

#include "Application.h" //To get settings.
#include "ExtruderTrain.h"
#include "Slice.h"
#include "settings/EnumSettings.h" //For EPlatformAdhesion.
#include "sliceDataStorage.h"
#include "utils/math.h"
#include "utils/DebugManager.h"

namespace cura
{

void Raft::generate(SliceDataStorage& storage)
{
    assert(
        storage.raft_base_outline.size() == 0 && storage.raft_interface_outline.size() == 0 && storage.raft_surface_outline.size() == 0
        && "Raft polygon isn't generated yet, so should be empty!");

    // 获取挤出机设置（这里包含了用户为特定挤出机设置的参数）
    const Settings& settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings.get<ExtruderTrain&>("raft_base_extruder_nr").settings_;
    constexpr bool include_support = true;
    constexpr bool dont_include_prime_tower = false; // Prime tower raft will be handled separately in 'storage.primeRaftOutline'; see below.
    const auto raft_base_margin = settings.get<coord_t>("raft_base_margin");
    const auto raft_interface_margin = settings.get<coord_t>("raft_interface_margin");
    const auto raft_surface_margin = settings.get<coord_t>("raft_surface_margin");

    // 检查是否为Spiralize模式且底层数为0的特殊情况
    // 在这种情况下，第一层只有螺旋壁而没有填充，因此Raft应该基于壁的路径而不是整个面积
    // 注意：这里必须从挤出机特定的settings中获取参数，而不是全局mesh_group_settings
    const bool magic_spiralize = settings.get<bool>("magic_spiralize");
    const size_t initial_bottom_layers = settings.get<size_t>("initial_bottom_layers");

    if (magic_spiralize && initial_bottom_layers == 0)
    {
        // Spiralize模式且无底层：基于第一层的螺旋壁生成优化的Raft
        spdlog::info("=== SPIRALIZE RAFT OPTIMIZATION ACTIVATED ===");
        spdlog::info("生成Spiralize模式优化Raft：基于第一层螺旋壁路径");

        // 写入调试文件确认代码被执行
        std::ofstream debug_file("spiralize_raft_debug.txt");
        debug_file << "SPIRALIZE RAFT OPTIMIZATION ACTIVATED\n";
        debug_file << "magic_spiralize: " << magic_spiralize << "\n";
        debug_file << "initial_bottom_layers: " << initial_bottom_layers << "\n";
        debug_file.close();

        // 收集所有第一层的螺旋壁路径
        Shape spiral_walls_combined;
        spdlog::info("开始收集第一层螺旋壁路径，mesh数量: {}", storage.meshes.size());

        for (const std::shared_ptr<SliceMeshStorage>& mesh : storage.meshes)
        {
            // 跳过填充网格和反悬垂网格
            if (mesh->settings.get<bool>("infill_mesh") || mesh->settings.get<bool>("anti_overhang_mesh"))
            {
                spdlog::info("跳过填充网格或反悬垂网格");
                continue;
            }

            spdlog::info("处理mesh，层数: {}", mesh->layers.size());

            // 检查第一层是否存在且有parts
            if (mesh->layers.size() > 0 && !mesh->layers[0].parts.empty())
            {
                spdlog::info("第一层parts数量: {}", mesh->layers[0].parts.size());

                for (const SliceLayerPart& part : mesh->layers[0].parts)
                {
                    spdlog::info("检查part - spiral_wall大小: {}, outline大小: {}",
                                part.spiral_wall.size(), part.outline.size());

                    // 使用螺旋壁路径，如果不存在则使用轮廓
                    if (!part.spiral_wall.empty())
                    {
                        spdlog::info("使用spiral_wall路径");
                        spiral_walls_combined.push_back(part.spiral_wall);
                    }
                    else if (!part.outline.empty())
                    {
                        // 如果没有螺旋壁，使用轮廓的内偏移作为替代
                        // 这种情况可能发生在某些边界条件下
                        spdlog::info("spiral_wall为空，使用outline内偏移");
                        const coord_t line_width = mesh->settings.get<coord_t>("wall_line_width_0");
                        Shape wall_path = part.outline.offset(-line_width / 2);
                        if (!wall_path.empty())
                        {
                            spiral_walls_combined.push_back(wall_path);
                        }
                    }
                    else
                    {
                        spdlog::warn("part既没有spiral_wall也没有outline");
                    }
                }
            }
            else
            {
                spdlog::warn("第一层为空或没有parts");
            }
        }

        if (!spiral_walls_combined.empty())
        {
            // 合并所有螺旋壁路径
            spiral_walls_combined = spiral_walls_combined.unionPolygons();

            spdlog::info("合并后的螺旋壁路径数量: {}", spiral_walls_combined.size());

            // 正确的Spiralize Raft优化逻辑：
            // 1. 向外扩展margin距离得到外边界
            // 2. 向内缩小margin距离得到内边界（如果可能的话）
            // 3. 使用difference操作得到环形区域
            Shape outer_boundary_base = spiral_walls_combined.offset(raft_base_margin, ClipperLib::jtRound);
            Shape inner_boundary_base = spiral_walls_combined.offset(-raft_base_margin, ClipperLib::jtRound);
            Shape raft_base_from_walls = outer_boundary_base.difference(inner_boundary_base);

            Shape outer_boundary_interface = spiral_walls_combined.offset(raft_interface_margin, ClipperLib::jtRound);
            Shape inner_boundary_interface = spiral_walls_combined.offset(-raft_interface_margin, ClipperLib::jtRound);
            Shape raft_interface_from_walls = outer_boundary_interface.difference(inner_boundary_interface);

            Shape outer_boundary_surface = spiral_walls_combined.offset(raft_surface_margin, ClipperLib::jtRound);
            Shape inner_boundary_surface = spiral_walls_combined.offset(-raft_surface_margin, ClipperLib::jtRound);
            Shape raft_surface_from_walls = outer_boundary_surface.difference(inner_boundary_surface);

            spdlog::info("Raft轮廓生成完成 - base: {}, interface: {}, surface: {}",
                        raft_base_from_walls.size(), raft_interface_from_walls.size(), raft_surface_from_walls.size());

            // 设置优化后的Raft轮廓
            storage.raft_base_outline = raft_base_from_walls;
            storage.raft_interface_outline = raft_interface_from_walls;
            storage.raft_surface_outline = raft_surface_from_walls;

            CURA_DEBUG(DEVELOPMENT, "Spiralize模式Raft优化完成：基于螺旋壁路径生成环形区域");
        }
        else
        {
            // 如果没有找到螺旋壁，回退到标准方法
            spdlog::warn("Spiralize模式下未找到螺旋壁路径，回退到标准Raft生成方法");
            storage.raft_base_outline = storage.raft_surface_outline = storage.raft_interface_outline = storage.getLayerOutlines(0, include_support, dont_include_prime_tower);
            storage.raft_base_outline = storage.raft_base_outline.offset(raft_base_margin, ClipperLib::jtRound);
            storage.raft_interface_outline = storage.raft_interface_outline.offset(raft_interface_margin, ClipperLib::jtRound);
            storage.raft_surface_outline = storage.raft_surface_outline.offset(raft_surface_margin, ClipperLib::jtRound);
        }
    }
    else
    {
        // 标准Raft生成方法：基于第一层的完整轮廓
        storage.raft_base_outline = storage.raft_surface_outline = storage.raft_interface_outline = storage.getLayerOutlines(0, include_support, dont_include_prime_tower);
        storage.raft_base_outline = storage.raft_base_outline.offset(raft_base_margin, ClipperLib::jtRound);
        storage.raft_interface_outline = storage.raft_interface_outline.offset(raft_interface_margin, ClipperLib::jtRound);
        storage.raft_surface_outline = storage.raft_surface_outline.offset(raft_surface_margin, ClipperLib::jtRound);
    }

    const coord_t shield_line_width_layer0 = settings.get<coord_t>("skirt_brim_line_width");
    const coord_t max_raft_distance = std::max(std::max(raft_base_margin, raft_interface_margin), raft_surface_margin);
    if (storage.draft_protection_shield.size() > 0)
    {
        Shape draft_shield_raft
            = storage.draft_protection_shield
                  .offset(shield_line_width_layer0) // start half a line width outside shield
                  .difference(storage.draft_protection_shield.offset(-max_raft_distance - shield_line_width_layer0 / 2, ClipperLib::jtRound)); // end distance inside shield
        storage.raft_base_outline = storage.raft_base_outline.unionPolygons(draft_shield_raft);
        storage.raft_surface_outline = storage.raft_surface_outline.unionPolygons(draft_shield_raft);
        storage.raft_interface_outline = storage.raft_interface_outline.unionPolygons(draft_shield_raft);
    }
    if (storage.ooze_shield.size() > 0 && storage.ooze_shield[0].size() > 0)
    {
        const Shape& ooze_shield = storage.ooze_shield[0];
        Shape ooze_shield_raft = ooze_shield
                                     .offset(shield_line_width_layer0) // start half a line width outside shield
                                     .difference(ooze_shield.offset(-max_raft_distance - shield_line_width_layer0 / 2, ClipperLib::jtRound)); // end distance inside shield
        storage.raft_base_outline = storage.raft_base_outline.unionPolygons(ooze_shield_raft);
        storage.raft_surface_outline = storage.raft_surface_outline.unionPolygons(ooze_shield_raft);
        storage.raft_interface_outline = storage.raft_interface_outline.unionPolygons(ooze_shield_raft);
    }

    const auto remove_inside_corners = [](Shape& outline, bool remove_inside_corners, coord_t smoothing, coord_t line_width)
    {
        if (remove_inside_corners)
        {
            // Make all parts convex. We could take the convex hull of the union of all parts, but that would
            // result in a massive brim if you would have models in all corners of the build plate. We instead
            // perform a convex hull operation on each polygon part separately, this will result in much smaller
            // raft islands. However, this might result in inside corners in the raft outline in the situation
            // where after the merge operations some parts become connected. To properly make sure that there are
            // no inside corners we perform a convex hull operation followed by a merge. If the number of polygon
            // parts no longer decrease we have found the polygons that have both no longer inside corners and
            // occupy the smallest possible area.
            outline = outline.unionPolygons();
            auto outline_parts = outline.unionPolygons().splitIntoParts();
            auto nr_of_parts = outline_parts.size();

            while (true)
            {
                outline.clear();

                for (auto& part : outline_parts)
                {
                    part.makeConvex();
                    outline.push_back(part);
                }

                outline = outline.unionPolygons();
                outline_parts = outline.splitIntoParts();
                const auto new_nr_of_parts = outline_parts.size();

                if (new_nr_of_parts > nr_of_parts)
                {
                    // from the recursive convex hull + merge operation the number of parts cannot logically increase
                    // if it does increase, and allow the loop to continue we might get into an infinite loop; so break out of the loop
                    // this might produce a raft with inside corners, but that is better than an infinite loop
                    spdlog::warn("Error while removing inside corners from raft; merge operation increased the number of parts");
                    assert(false);
                    break;
                }

                if (new_nr_of_parts == nr_of_parts)
                {
                    break;
                }
                nr_of_parts = new_nr_of_parts;
            }
        }
        else
        {
            // Closing operation for smoothing:
            outline = outline.offset(smoothing, ClipperLib::jtRound).offset(-smoothing, ClipperLib::jtRound);

            // Opening operation to get rid of articfacts created by the closing operation:
            outline = outline.offset(-line_width, ClipperLib::jtRound).offset(line_width, ClipperLib::jtRound);
        }
    };
    const auto nominal_raft_line_width = settings.get<coord_t>("skirt_brim_line_width");
    remove_inside_corners(storage.raft_base_outline, settings.get<bool>("raft_base_remove_inside_corners"), settings.get<coord_t>("raft_base_smoothing"), nominal_raft_line_width);
    remove_inside_corners(
        storage.raft_interface_outline,
        settings.get<bool>("raft_interface_remove_inside_corners"),
        settings.get<coord_t>("raft_interface_smoothing"),
        nominal_raft_line_width);
    remove_inside_corners(
        storage.raft_surface_outline,
        settings.get<bool>("raft_surface_remove_inside_corners"),
        settings.get<coord_t>("raft_surface_smoothing"),
        nominal_raft_line_width);
}

coord_t Raft::getTotalThickness()
{
    const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;
    const Settings& base_train = mesh_group_settings.get<ExtruderTrain&>("raft_base_extruder_nr").settings_;
    const Settings& interface_train = mesh_group_settings.get<ExtruderTrain&>("raft_interface_extruder_nr").settings_;
    const Settings& surface_train = mesh_group_settings.get<ExtruderTrain&>("raft_surface_extruder_nr").settings_;
    return base_train.get<coord_t>("raft_base_thickness") + interface_train.get<size_t>("raft_interface_layers") * interface_train.get<coord_t>("raft_interface_thickness")
         + interface_train.get<coord_t>("raft_interface_z_offset") + surface_train.get<size_t>("raft_surface_layers") * surface_train.get<coord_t>("raft_surface_thickness")
         + interface_train.get<coord_t>("raft_surface_z_offset");
}

coord_t Raft::getZdiffBetweenRaftAndLayer0()
{
    const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;
    const ExtruderTrain& train = mesh_group_settings.get<ExtruderTrain&>("raft_surface_extruder_nr");
    if (mesh_group_settings.get<EPlatformAdhesion>("adhesion_type") != EPlatformAdhesion::RAFT)
    {
        return 0;
    }
    const coord_t airgap = std::max(coord_t(0), train.settings_.get<coord_t>("raft_airgap"));
    return airgap;
}

size_t Raft::getFillerLayerCount()
{
    const coord_t normal_layer_height = Application::getInstance().current_slice_->scene.current_mesh_group->settings.get<coord_t>("layer_height");
    return round_divide(getZdiffBetweenRaftAndLayer0(), normal_layer_height);
}

coord_t Raft::getFillerLayerHeight()
{
    const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;
    if (mesh_group_settings.get<EPlatformAdhesion>("adhesion_type") != EPlatformAdhesion::RAFT)
    {
        const coord_t normal_layer_height = mesh_group_settings.get<coord_t>("layer_height");
        return normal_layer_height;
    }

    return round_divide(getZdiffBetweenRaftAndLayer0(), getFillerLayerCount());
}

size_t Raft::getTotalExtraLayers()
{
    return getBaseLayers() + getInterfaceLayers() + getSurfaceLayers() + getFillerLayerCount();
}

size_t Raft::getBaseLayers()
{
    const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;
    if (mesh_group_settings.get<EPlatformAdhesion>("adhesion_type") != EPlatformAdhesion::RAFT)
    {
        return 0;
    }
    return 1;
}

size_t Raft::getInterfaceLayers()
{
    return getLayersAmount("raft_interface_extruder_nr", "raft_interface_layers");
}

size_t Raft::getSurfaceLayers()
{
    return getLayersAmount("raft_surface_extruder_nr", "raft_surface_layers");
}

Raft::LayerType Raft::getLayerType(LayerIndex layer_index)
{
    const auto airgap = Raft::getFillerLayerCount();
    const auto interface_layers = Raft::getInterfaceLayers();
    const auto surface_layers = Raft::getSurfaceLayers();

    if (layer_index < -LayerIndex(airgap + surface_layers + interface_layers))
    {
        return LayerType::RaftBase;
    }
    if (layer_index < -LayerIndex(airgap + surface_layers))
    {
        return LayerType::RaftInterface;
    }
    if (layer_index < -LayerIndex(airgap))
    {
        return LayerType::RaftSurface;
    }
    if (layer_index < LayerIndex(0))
    {
        return LayerType::Airgap;
    }
    return LayerType::Model;
}

size_t Raft::getLayersAmount(const std::string& extruder_nr_setting_name, const std::string& target_raft_section)
{
    const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;
    if (mesh_group_settings.get<EPlatformAdhesion>("adhesion_type") != EPlatformAdhesion::RAFT)
    {
        return 0;
    }

    const ExtruderTrain& train = mesh_group_settings.get<ExtruderTrain&>(extruder_nr_setting_name);
    return train.settings_.get<size_t>(target_raft_section);
}


} // namespace cura
