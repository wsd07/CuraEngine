#ifndef CRASH_SAFE_DEBUG_H
#define CRASH_SAFE_DEBUG_H

#include <iostream>
#include <csignal>
#include <cstdlib>
#include <cassert>
#include <string>

// 前向声明，避免直接包含spdlog
namespace spdlog {
    class logger;
    void error(const std::string& msg);
    std::shared_ptr<logger> get(const std::string& name);
}

namespace fmt {
    template<typename... Args>
    std::string format(const std::string& format_str, Args&&... args);
}

namespace cura
{

/**
 * @brief 崩溃安全的错误处理工具
 * 
 * 提供立即刷新的错误日志和崩溃处理机制，确保在程序崩溃时
 * 能够输出详细的错误信息用于调试。
 */
class CrashSafeDebug
{
public:
    /**
     * @brief 初始化崩溃处理器
     * 
     * 设置信号处理器来捕获段错误和断言失败，
     * 确保在崩溃时能输出有用的调试信息。
     */
    static void initialize();

    /**
     * @brief 立即刷新的错误日志
     *
     * 同时输出到spdlog和stderr，并立即刷新缓冲区，
     * 确保在程序崩溃时信息不会丢失。
     */
    static void errorFlush(const std::string& message);

    /**
     * @brief 格式化的错误日志
     */
    static void errorFlushF(const char* format, ...);

    /**
     * @brief 简单的错误日志（C字符串）
     */
    static void errorFlushSimple(const char* message);

    /**
     * @brief 带断言的错误检查
     *
     * 如果条件失败，输出详细错误信息并触发断言，
     * 确保程序在可控状态下崩溃并输出调试信息。
     */
    static void assertWithInfo(bool condition, const char* condition_str, const char* message);

    /**
     * @brief 条件错误检查
     *
     * 如果条件失败，输出错误信息但不崩溃，
     * 返回false表示检查失败。
     */
    static bool checkWithError(bool condition, const char* message);

private:
    /**
     * @brief 崩溃信号处理器
     */
    static void crashHandler(int signal);
    
    /**
     * @brief 打印调用栈（平台相关）
     */
    static void printStackTrace();
    
    static bool initialized_;
};

} // namespace cura

// 便捷宏定义
#define CURA_ERROR_FLUSH(msg) cura::CrashSafeDebug::errorFlush(msg)
#define CURA_ERROR_FLUSH_F(...) cura::CrashSafeDebug::errorFlushF(__VA_ARGS__)

#define CURA_ASSERT_WITH_INFO(condition, message) \
    cura::CrashSafeDebug::assertWithInfo((condition), #condition, message)

#define CURA_CHECK_WITH_ERROR(condition, message) \
    cura::CrashSafeDebug::checkWithError((condition), message)

#endif // CRASH_SAFE_DEBUG_H
