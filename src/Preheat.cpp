// Copyright (c) 2022 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher

#include "Preheat.h"

#include <spdlog/spdlog.h>

#include "Application.h" //To get settings.
#include "ExtruderTrain.h"
#include "Slice.h"
#include "settings/FlowTempGraph.h"
#include "settings/HeightParameterGraph.h"
#include "settings/types/Ratio.h"
#include "settings/EnumSettings.h"
#include "raft.h"

namespace cura
{

Duration Preheat::getTimeToGoFromTempToTemp(const size_t extruder, const Temperature& temp_before, const Temperature& temp_after, const bool during_printing)
{
    const Settings& extruder_settings = Application::getInstance().current_slice_->scene.extruders[extruder].settings_;
    Duration time;
    if (temp_after > temp_before)
    {
        Temperature heat_up_speed = extruder_settings.get<Temperature>("machine_nozzle_heat_up_speed");
        if (during_printing)
        {
            heat_up_speed -= extruder_settings.get<Temperature>("material_extrusion_cool_down_speed");
        }
        time = (temp_after - temp_before) / heat_up_speed;
    }
    else
    {
        Temperature cool_down_speed = extruder_settings.get<Temperature>("machine_nozzle_cool_down_speed");
        if (during_printing)
        {
            cool_down_speed += extruder_settings.get<Temperature>("material_extrusion_cool_down_speed");
        }
        time = (temp_before - temp_after) / cool_down_speed;
    }
    return std::max(0.0_s, time);
}

Temperature Preheat::getTemp(const size_t extruder, const bool is_initial_layer)
{
    const Settings& extruder_settings = Application::getInstance().current_slice_->scene.extruders[extruder].settings_;
    if (is_initial_layer && extruder_settings.get<Temperature>("material_print_temperature_layer_0") != 0)
    {
        return extruder_settings.get<Temperature>("material_print_temperature_layer_0");
    }
    return extruder_settings.get<Temperature>("material_print_temperature");
}

Temperature Preheat::getTemp(const size_t extruder, const bool is_initial_layer, const coord_t layer_z)
{
    const Settings& extruder_settings = Application::getInstance().current_slice_->scene.extruders[extruder].settings_;

    // === 首层温度保护：首层参数不受可变参数影响 ===
    if (is_initial_layer)
    {
        Temperature layer_0_temp = extruder_settings.get<Temperature>("material_print_temperature_layer_0");
        if (layer_0_temp != 0)
        {
            spdlog::debug("首层温度保护: 使用layer_0温度 {:.0f}°C", static_cast<double>(layer_0_temp));
            return layer_0_temp;
        }
        else
        {
            // 如果没有设置layer_0温度，使用基础温度
            Temperature base_temp = extruder_settings.get<Temperature>("material_print_temperature");
            spdlog::debug("首层温度保护: 使用基础温度 {:.0f}°C", static_cast<double>(base_temp));
            return base_temp;
        }
    }

    // === 用户定义温度控制（仅非首层） ===
    if (extruder_settings.get<bool>("user_temperature_definition_enable"))
    {
        auto user_temperature_definition = extruder_settings.get<HeightParameterGraph>("user_temperature_definition");
        if (!user_temperature_definition.isEmpty())
        {
            // 计算模型实体高度：扣除raft厚度和间隙
            coord_t model_height = layer_z;

            const Settings& mesh_group_settings = Application::getInstance().current_slice_->scene.current_mesh_group->settings;
            if (mesh_group_settings.get<EPlatformAdhesion>("adhesion_type") == EPlatformAdhesion::RAFT)
            {
                // 获取raft总厚度
                coord_t raft_total_thickness = Raft::getTotalThickness();

                // 获取raft间隙
                const ExtruderTrain& raft_surface_train = mesh_group_settings.get<ExtruderTrain&>("raft_surface_extruder_nr");
                coord_t raft_airgap = raft_surface_train.settings_.get<coord_t>("raft_airgap");

                // 计算模型实体高度
                model_height = layer_z - raft_total_thickness - raft_airgap;

                spdlog::debug("Raft计算: 层Z={:.2f}mm, Raft厚度={:.2f}mm, Raft间隙={:.2f}mm, 模型高度={:.2f}mm",
                             INT2MM(layer_z), INT2MM(raft_total_thickness), INT2MM(raft_airgap), INT2MM(model_height));
            }

            // 如果模型高度为负数，说明还在raft层，使用基础温度
            if (model_height < 0)
            {
                Temperature base_temp = extruder_settings.get<Temperature>("material_print_temperature");
                spdlog::debug("Raft层温度: 使用基础温度 {:.0f}°C", static_cast<double>(base_temp));
                return base_temp;
            }

            // 获取基础温度作为默认值
            Temperature base_temp = extruder_settings.get<Temperature>("material_print_temperature");

            // 根据模型实体高度获取用户定义的温度
            double user_temp = user_temperature_definition.getParameter(model_height, base_temp);

            spdlog::debug("用户定义温度控制: 模型高度={:.2f}mm, 温度={:.0f}°C", INT2MM(model_height), user_temp);
            return Temperature(user_temp);
        }
    }

    // === 原有温度逻辑 ===
    if (is_initial_layer && extruder_settings.get<Temperature>("material_print_temperature_layer_0") != 0)
    {
        return extruder_settings.get<Temperature>("material_print_temperature_layer_0");
    }
    return extruder_settings.get<Temperature>("material_print_temperature");
}

Preheat::WarmUpResult Preheat::getWarmUpPointAfterCoolDown(double time_window, unsigned int extruder, double temp_start, double temp_mid, double temp_end, bool during_printing)
{
    WarmUpResult result;
    const Settings& extruder_settings = Application::getInstance().current_slice_->scene.extruders[extruder].settings_;
    Temperature cool_down_speed = extruder_settings.get<Temperature>("machine_nozzle_cool_down_speed");
    if (during_printing)
    {
        cool_down_speed += extruder_settings.get<Temperature>("material_extrusion_cool_down_speed");
    }
    const Duration time_to_cooldown_1_degree = 1.0 / cool_down_speed;
    Temperature heat_up_speed = extruder_settings.get<Temperature>("machine_nozzle_heat_up_speed");
    if (during_printing)
    {
        heat_up_speed -= extruder_settings.get<Temperature>("material_extrusion_cool_down_speed");
    }
    const Duration time_to_heatup_1_degree = 1.0 / heat_up_speed;
    result.total_time_window = time_window;

    //                    ,temp_end
    //                   /                                    .
    //     ,temp_start  /                                     .
    //      \ ' ' ' ' '/ ' ' '> outer_temp                    .
    //       \________/                                       .
    //               "-> temp_mid
    //      ^^^^^^^^^^
    //      limited_time_window
    double outer_temp;
    double limited_time_window;
    if (temp_start < temp_end)
    { // extra time needed during heating
        double extra_heatup_time = (temp_end - temp_start) * time_to_heatup_1_degree;
        result.heating_time = extra_heatup_time;
        limited_time_window = time_window - extra_heatup_time;
        outer_temp = temp_start;
    }
    else
    {
        double extra_cooldown_time = (temp_start - temp_end) * time_to_cooldown_1_degree;
        result.heating_time = 0;
        limited_time_window = time_window - extra_cooldown_time;
        outer_temp = temp_end;
    }
    if (limited_time_window < 0.0)
    {
        result.heating_time = 0.0;
        result.lowest_temperature = std::min(temp_start, temp_end);
        return result;
    }

    double time_ratio_cooldown_heatup = time_to_cooldown_1_degree / time_to_heatup_1_degree;
    double time_to_heat_from_standby_to_print_temp = getTimeToGoFromTempToTemp(extruder, temp_mid, outer_temp, during_printing);
    double time_needed_to_reach_standby_temp = time_to_heat_from_standby_to_print_temp * (1.0 + time_ratio_cooldown_heatup);
    if (time_needed_to_reach_standby_temp < limited_time_window)
    {
        result.heating_time += time_to_heat_from_standby_to_print_temp;
        result.lowest_temperature = temp_mid;
    }
    else
    {
        result.heating_time += limited_time_window * time_to_heatup_1_degree / (time_to_cooldown_1_degree + time_to_heatup_1_degree);
        result.lowest_temperature = std::max(temp_mid, temp_end - result.heating_time / time_to_heatup_1_degree);
    }

    if (result.heating_time > time_window || result.heating_time < 0.0)
    {
        spdlog::warn("getWarmUpPointAfterCoolDown returns result outside of the time window!");
    }
    return result;
}

Preheat::CoolDownResult Preheat::getCoolDownPointAfterWarmUp(double time_window, unsigned int extruder, double temp_start, double temp_mid, double temp_end, bool during_printing)
{
    CoolDownResult result;
    const Settings& extruder_settings = Application::getInstance().current_slice_->scene.extruders[extruder].settings_;
    Temperature cool_down_speed = extruder_settings.get<Temperature>("machine_nozzle_cool_down_speed");
    if (during_printing)
    {
        cool_down_speed += extruder_settings.get<Temperature>("material_extrusion_cool_down_speed");
    }
    const Duration time_to_cooldown_1_degree = 1.0 / cool_down_speed;
    Temperature heat_up_speed = extruder_settings.get<Temperature>("machine_nozzle_heat_up_speed");
    if (during_printing)
    {
        heat_up_speed -= extruder_settings.get<Temperature>("material_extrusion_cool_down_speed");
    }
    const Duration time_to_heatup_1_degree = 1.0 / heat_up_speed;

    assert(temp_start != -1 && temp_mid != -1 && temp_end != -1 && "temperatures must be initialized!");

    result.total_time_window = time_window;

    //      limited_time_window
    //     :^^^^^^^^^^^^:
    //     :  ________. : . . .> temp_mid
    //     : /        \ :                                     .
    //     :/ . . . . .\:. . .> outer_temp                    .
    //     ^temp_start  \                                     .
    //                   \                                    .
    //                    ^temp_end
    double outer_temp;
    double limited_time_window;
    if (temp_start < temp_end)
    { // extra time needed during heating
        double extra_heatup_time = (temp_end - temp_start) * time_to_heatup_1_degree;
        result.cooling_time = 0;
        limited_time_window = time_window - extra_heatup_time;
        outer_temp = temp_end;
    }
    else
    {
        double extra_cooldown_time = (temp_start - temp_end) * time_to_cooldown_1_degree;
        result.cooling_time = extra_cooldown_time;
        limited_time_window = time_window - extra_cooldown_time;
        outer_temp = temp_start;
    }
    if (limited_time_window < 0.0)
    {
        result.cooling_time = 0.0;
        result.highest_temperature = std::max(temp_start, temp_end);
        return result;
    }
    double time_ratio_cooldown_heatup = time_to_cooldown_1_degree / time_to_heatup_1_degree;
    double cool_down_time = getTimeToGoFromTempToTemp(extruder, temp_mid, outer_temp, during_printing);
    double time_needed_to_reach_temp1 = cool_down_time * (1.0 + time_ratio_cooldown_heatup);
    if (time_needed_to_reach_temp1 < limited_time_window)
    {
        result.cooling_time += cool_down_time;
        result.highest_temperature = temp_mid;
    }
    else
    {
        result.cooling_time += limited_time_window * time_to_heatup_1_degree / (time_to_cooldown_1_degree + time_to_heatup_1_degree);
        result.highest_temperature = std::min(temp_mid, temp_end + result.cooling_time / time_to_cooldown_1_degree);
    }

    if (result.cooling_time > time_window || result.cooling_time < 0.0)
    {
        spdlog::warn("getCoolDownPointAfterWarmUp returns result outside of the time window!");
    }
    return result;
}

} // namespace cura
