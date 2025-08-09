// Copyright (c) 2024 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#include "utils/DebugManager.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <unordered_map>

namespace cura
{

DebugManager& DebugManager::getInstance()
{
    static DebugManager instance;
    return instance;
}

void DebugManager::enableCategory(Category category)
{
    if (category == Category::ALL)
    {
        enableAll();
    }
    else
    {
        enabled_categories_.insert(category);
    }
}

void DebugManager::disableCategory(Category category)
{
    if (category == Category::ALL)
    {
        disableAll();
    }
    else
    {
        enabled_categories_.erase(category);
    }
}

bool DebugManager::isCategoryEnabled(Category category) const
{
    return enabled_categories_.find(category) != enabled_categories_.end();
}

void DebugManager::enableAll()
{
    enabled_categories_.clear();
    enabled_categories_.insert(Category::BEADING_STRATEGY);
    enabled_categories_.insert(Category::SKELETAL_TRAPEZOIDATION);
    enabled_categories_.insert(Category::WALL_COMPUTATION);
    enabled_categories_.insert(Category::INFILL);
    enabled_categories_.insert(Category::SUPPORT);
    enabled_categories_.insert(Category::TREE_SUPPORT);
    enabled_categories_.insert(Category::PATH_PLANNING);
    enabled_categories_.insert(Category::LAYER_PLAN);
    enabled_categories_.insert(Category::TRAVEL_OPTIMIZATION);
    enabled_categories_.insert(Category::SEAM_PLACEMENT);
    enabled_categories_.insert(Category::GEOMETRY);
    enabled_categories_.insert(Category::POLYGON_PROCESSING);
    enabled_categories_.insert(Category::MESH_PROCESSING);
    enabled_categories_.insert(Category::SLICING);
    enabled_categories_.insert(Category::SETTINGS);
    enabled_categories_.insert(Category::ADAPTIVE_LAYERS);
    enabled_categories_.insert(Category::FLOW_COMPENSATION);
    enabled_categories_.insert(Category::GCODE_GENERATION);
    enabled_categories_.insert(Category::GCODE_EXPORT);
    enabled_categories_.insert(Category::COMMUNICATION);
    enabled_categories_.insert(Category::PLUGINS);
    enabled_categories_.insert(Category::PERFORMANCE);
    enabled_categories_.insert(Category::MEMORY);
    enabled_categories_.insert(Category::PROGRESS);
    enabled_categories_.insert(Category::DEVELOPMENT);
    enabled_categories_.insert(Category::TESTING);
}

void DebugManager::disableAll()
{
    enabled_categories_.clear();
}

void DebugManager::enableOnly(Category category)
{
    disableAll();
    if (category != Category::ALL)
    {
        enableCategory(category);
    }
    else
    {
        enableAll();
    }
}

void DebugManager::enableFromString(const std::string& categories)
{
    std::stringstream ss(categories);
    std::string category_name;
    
    while (std::getline(ss, category_name, ','))
    {
        // 去除空格
        category_name.erase(std::remove_if(category_name.begin(), category_name.end(), ::isspace), category_name.end());
        
        if (!category_name.empty())
        {
            Category category = getCategoryFromString(category_name);
            if (category != Category::ALL || category_name == "ALL")
            {
                enableCategory(category);
            }
        }
    }
}

std::string DebugManager::getCategoryName(Category category)
{
    static const std::unordered_map<Category, std::string> category_names = {
        {Category::BEADING_STRATEGY, "BEADING_STRATEGY"},
        {Category::SKELETAL_TRAPEZOIDATION, "SKELETAL_TRAPEZOIDATION"},
        {Category::WALL_COMPUTATION, "WALL_COMPUTATION"},
        {Category::INFILL, "INFILL"},
        {Category::SUPPORT, "SUPPORT"},
        {Category::TREE_SUPPORT, "TREE_SUPPORT"},
        {Category::PATH_PLANNING, "PATH_PLANNING"},
        {Category::LAYER_PLAN, "LAYER_PLAN"},
        {Category::TRAVEL_OPTIMIZATION, "TRAVEL_OPTIMIZATION"},
        {Category::SEAM_PLACEMENT, "SEAM_PLACEMENT"},
        {Category::GEOMETRY, "GEOMETRY"},
        {Category::POLYGON_PROCESSING, "POLYGON_PROCESSING"},
        {Category::MESH_PROCESSING, "MESH_PROCESSING"},
        {Category::SLICING, "SLICING"},
        {Category::SETTINGS, "SETTINGS"},
        {Category::ADAPTIVE_LAYERS, "ADAPTIVE_LAYERS"},
        {Category::FLOW_COMPENSATION, "FLOW_COMPENSATION"},
        {Category::GCODE_GENERATION, "GCODE_GENERATION"},
        {Category::GCODE_EXPORT, "GCODE_EXPORT"},
        {Category::COMMUNICATION, "COMMUNICATION"},
        {Category::PLUGINS, "PLUGINS"},
        {Category::PERFORMANCE, "PERFORMANCE"},
        {Category::MEMORY, "MEMORY"},
        {Category::PROGRESS, "PROGRESS"},
        {Category::DEVELOPMENT, "DEVELOPMENT"},
        {Category::TESTING, "TESTING"},
        {Category::ALL, "ALL"}
    };
    
    auto it = category_names.find(category);
    return (it != category_names.end()) ? it->second : "UNKNOWN";
}

DebugManager::Category DebugManager::getCategoryFromString(const std::string& name)
{
    static const std::unordered_map<std::string, Category> string_to_category = {
        {"BEADING_STRATEGY", Category::BEADING_STRATEGY},
        {"SKELETAL_TRAPEZOIDATION", Category::SKELETAL_TRAPEZOIDATION},
        {"WALL_COMPUTATION", Category::WALL_COMPUTATION},
        {"INFILL", Category::INFILL},
        {"SUPPORT", Category::SUPPORT},
        {"TREE_SUPPORT", Category::TREE_SUPPORT},
        {"PATH_PLANNING", Category::PATH_PLANNING},
        {"LAYER_PLAN", Category::LAYER_PLAN},
        {"TRAVEL_OPTIMIZATION", Category::TRAVEL_OPTIMIZATION},
        {"SEAM_PLACEMENT", Category::SEAM_PLACEMENT},
        {"GEOMETRY", Category::GEOMETRY},
        {"POLYGON_PROCESSING", Category::POLYGON_PROCESSING},
        {"MESH_PROCESSING", Category::MESH_PROCESSING},
        {"SLICING", Category::SLICING},
        {"SETTINGS", Category::SETTINGS},
        {"ADAPTIVE_LAYERS", Category::ADAPTIVE_LAYERS},
        {"FLOW_COMPENSATION", Category::FLOW_COMPENSATION},
        {"GCODE_GENERATION", Category::GCODE_GENERATION},
        {"GCODE_EXPORT", Category::GCODE_EXPORT},
        {"COMMUNICATION", Category::COMMUNICATION},
        {"PLUGINS", Category::PLUGINS},
        {"PERFORMANCE", Category::PERFORMANCE},
        {"MEMORY", Category::MEMORY},
        {"PROGRESS", Category::PROGRESS},
        {"DEVELOPMENT", Category::DEVELOPMENT},
        {"TESTING", Category::TESTING},
        {"ALL", Category::ALL}
    };
    
    auto it = string_to_category.find(name);
    return (it != string_to_category.end()) ? it->second : Category::DEVELOPMENT;
}

void DebugManager::printAvailableCategories() const
{
    std::cout << "Available debug categories:" << std::endl;
    std::cout << "=== 核心算法 ===" << std::endl;
    std::cout << "  BEADING_STRATEGY        - BeadingStrategy相关算法" << std::endl;
    std::cout << "  SKELETAL_TRAPEZOIDATION - 骨架梯形化算法" << std::endl;
    std::cout << "  WALL_COMPUTATION        - 墙体计算" << std::endl;
    std::cout << "  INFILL                  - 填充算法" << std::endl;
    std::cout << "  SUPPORT                 - 支撑算法" << std::endl;
    std::cout << "  TREE_SUPPORT            - 树状支撑" << std::endl;
    std::cout << std::endl;
    std::cout << "=== 路径规划 ===" << std::endl;
    std::cout << "  PATH_PLANNING           - 路径规划" << std::endl;
    std::cout << "  LAYER_PLAN              - 层规划" << std::endl;
    std::cout << "  TRAVEL_OPTIMIZATION     - 行程优化" << std::endl;
    std::cout << "  SEAM_PLACEMENT          - 接缝放置" << std::endl;
    std::cout << std::endl;
    std::cout << "=== 几何处理 ===" << std::endl;
    std::cout << "  GEOMETRY                - 几何计算" << std::endl;
    std::cout << "  POLYGON_PROCESSING      - 多边形处理" << std::endl;
    std::cout << "  MESH_PROCESSING         - 网格处理" << std::endl;
    std::cout << "  SLICING                 - 切片" << std::endl;
    std::cout << std::endl;
    std::cout << "=== 设置和配置 ===" << std::endl;
    std::cout << "  SETTINGS                - 设置系统" << std::endl;
    std::cout << "  ADAPTIVE_LAYERS         - 自适应层高" << std::endl;
    std::cout << "  FLOW_COMPENSATION       - 流量补偿" << std::endl;
    std::cout << std::endl;
    std::cout << "=== 输出生成 ===" << std::endl;
    std::cout << "  GCODE_GENERATION        - G代码生成" << std::endl;
    std::cout << "  GCODE_EXPORT            - G代码导出" << std::endl;
    std::cout << std::endl;
    std::cout << "=== 通信和插件 ===" << std::endl;
    std::cout << "  COMMUNICATION           - 通信系统" << std::endl;
    std::cout << "  PLUGINS                 - 插件系统" << std::endl;
    std::cout << std::endl;
    std::cout << "=== 性能和调试 ===" << std::endl;
    std::cout << "  PERFORMANCE             - 性能分析" << std::endl;
    std::cout << "  MEMORY                  - 内存管理" << std::endl;
    std::cout << "  PROGRESS                - 进度报告" << std::endl;
    std::cout << std::endl;
    std::cout << "=== 开发和测试 ===" << std::endl;
    std::cout << "  DEVELOPMENT             - 开发调试" << std::endl;
    std::cout << "  TESTING                 - 测试相关" << std::endl;
    std::cout << "  ALL                     - 所有分类" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: --debug-categories CATEGORY1,CATEGORY2,..." << std::endl;
    std::cout << "Example: --debug-categories FLOW_COMPENSATION,BEADING_STRATEGY" << std::endl;
}

} // namespace cura
