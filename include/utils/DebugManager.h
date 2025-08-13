// Copyright (c) 2024 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef DEBUG_MANAGER_H
#define DEBUG_MANAGER_H

#include <spdlog/spdlog.h>
#include <unordered_set>
#include <string>

namespace cura
{

/*!
 * 调试信息管控系统
 * 
 * 提供分类的调试信息控制，允许开发者：
 * 1. 按功能模块开启/关闭调试信息
 * 2. 在开发新功能时只显示相关调试信息
 * 3. 统一管理所有调试输出的格式和级别
 */
class DebugManager
{
public:
    /*!
     * 调试分类枚举
     * 每个分类对应一个功能模块或开发领域
     */
    enum class Category
    {
        // === 核心算法 ===
        BEADING_STRATEGY,           // BeadingStrategy相关算法
        SKELETAL_TRAPEZOIDATION,    // 骨架梯形化算法
        WALL_COMPUTATION,           // 墙体计算
        INFILL,                     // 填充算法
        SUPPORT,                    // 支撑算法
        TREE_SUPPORT,               // 树状支撑
        
        // === 路径规划 ===
        PATH_PLANNING,              // 路径规划
        LAYER_PLAN,                 // 层规划
        TRAVEL_OPTIMIZATION,        // 行程优化
        SEAM_PLACEMENT,             // 接缝放置
        COMB,                       // Combing路径规划
        
        // === 几何处理 ===
        GEOMETRY,                   // 几何计算
        POLYGON_PROCESSING,         // 多边形处理
        MESH_PROCESSING,            // 网格处理
        SLICING,                    // 切片
        
        // === 设置和配置 ===
        SETTINGS,                   // 设置系统
        ADAPTIVE_LAYERS,            // 自适应层高
        FLOW_COMPENSATION,          // 流量补偿
        
        // === 输出生成 ===
        GCODE_GENERATION,           // G代码生成
        GCODE_EXPORT,               // G代码导出
        
        // === 通信和插件 ===
        COMMUNICATION,              // 通信系统
        PLUGINS,                    // 插件系统
        
        // === 性能和调试 ===
        PERFORMANCE,                // 性能分析
        MEMORY,                     // 内存管理
        PROGRESS,                   // 进度报告
        
        // === 开发和测试 ===
        DEVELOPMENT,                // 开发调试
        TESTING,                    // 测试相关
        
        ALL                         // 所有分类（用于全局控制）
    };

    /*!
     * 获取单例实例
     */
    static DebugManager& getInstance();

    /*!
     * 启用指定分类的调试信息
     */
    void enableCategory(Category category);

    /*!
     * 禁用指定分类的调试信息
     */
    void disableCategory(Category category);

    /*!
     * 检查指定分类是否启用
     */
    bool isCategoryEnabled(Category category) const;

    /*!
     * 启用所有分类
     */
    void enableAll();

    /*!
     * 禁用所有分类
     */
    void disableAll();

    /*!
     * 只启用指定分类，禁用其他所有分类
     */
    void enableOnly(Category category);

    /*!
     * 从字符串启用分类（用于命令行参数）
     */
    void enableFromString(const std::string& categories);

    /*!
     * 获取分类名称
     */
    static std::string getCategoryName(Category category);

    /*!
     * 从字符串获取分类
     */
    static Category getCategoryFromString(const std::string& name);

    /*!
     * 打印所有可用分类
     */
    void printAvailableCategories() const;

private:
    DebugManager() = default;
    std::unordered_set<Category> enabled_categories_;
};

/*!
 * 便捷宏定义，用于分类调试输出
 */
#define CURA_DEBUG(category, ...) \
    do { \
        if (cura::DebugManager::getInstance().isCategoryEnabled(cura::DebugManager::Category::category)) { \
            spdlog::debug(__VA_ARGS__); \
        } \
    } while(0)

#define CURA_DEBUG_IF(category, condition, ...) \
    do { \
        if ((condition) && cura::DebugManager::getInstance().isCategoryEnabled(cura::DebugManager::Category::category)) { \
            spdlog::debug(__VA_ARGS__); \
        } \
    } while(0)

/*!
 * 信息级别输出（不受分类控制，用于重要的用户信息）
 */
#define CURA_INFO(...) spdlog::info(__VA_ARGS__)

/*!
 * 警告级别输出（不受分类控制，用于警告信息）
 */
#define CURA_WARN(...) spdlog::warn(__VA_ARGS__)

/*!
 * 错误级别输出（不受分类控制，用于错误信息）
 */
#define CURA_ERROR(...) spdlog::error(__VA_ARGS__)

} // namespace cura

#endif // DEBUG_MANAGER_H
