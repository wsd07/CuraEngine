#include "utils/CrashSafeDebug.h"

#include <iostream>
#include <cstdlib>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

// 平台相关的调用栈打印
#ifdef __APPLE__
    #include <execinfo.h>
    #include <cxxabi.h>
#elif defined(__linux__)
    #include <execinfo.h>
    #include <cxxabi.h>
#elif defined(_WIN32)
    #include <windows.h>
    #include <dbghelp.h>
    #pragma comment(lib, "dbghelp.lib")
#endif

namespace cura
{

// 线程安全的调试输出mutex
static std::mutex debug_mutex;

bool CrashSafeDebug::initialized_ = false;

void CrashSafeDebug::initialize()
{
    if (initialized_)
    {
        return;
    }
    
    // 设置信号处理器
    signal(SIGSEGV, crashHandler);  // 段错误
    signal(SIGABRT, crashHandler);  // 断言失败
    signal(SIGFPE, crashHandler);   // 浮点异常
    signal(SIGILL, crashHandler);   // 非法指令
    
#ifndef _WIN32
    signal(SIGBUS, crashHandler);   // 总线错误 (Unix/Linux)
#endif
    
    initialized_ = true;

    errorFlush("CrashSafeDebug initialized - 崩溃安全调试已启用");
}

void CrashSafeDebug::errorFlush(const std::string& message)
{
    // 线程安全的调试输出
    std::lock_guard<std::mutex> lock(debug_mutex);

    // 输出到spdlog并立即刷新
    spdlog::error("{}", message);
    if (auto logger = spdlog::get("default"))
    {
        logger->flush();
    }

    // 同时输出到stderr并立即刷新
    std::cerr << "[ERROR] " << message << std::endl;
    std::cerr.flush();

    // 强制刷新所有输出流
    std::cout.flush();
    fflush(stdout);
    fflush(stderr);
}

void CrashSafeDebug::errorFlushF(const char* format, ...)
{
    // 线程安全的格式化输出
    std::lock_guard<std::mutex> lock(debug_mutex);

    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // 直接输出，避免重复加锁
    std::string message(buffer);

    // 输出到spdlog并立即刷新
    spdlog::error("{}", message);
    if (auto logger = spdlog::get("default"))
    {
        logger->flush();
    }

    // 同时输出到stderr并立即刷新
    std::cerr << "[ERROR] " << message << std::endl;
    std::cerr.flush();

    // 强制刷新所有输出流
    std::cout.flush();
    fflush(stdout);
    fflush(stderr);
}

void CrashSafeDebug::errorFlushSimple(const char* message)
{
    errorFlush(std::string(message));
}

void CrashSafeDebug::assertWithInfo(bool condition, const char* condition_str, const char* message)
{
    if (!condition)
    {
        errorFlush(std::string("ASSERTION FAILED: ") + condition_str);
        errorFlush(message);
        errorFlush("程序将在此处停止以便调试");

        // 输出调用栈信息（如果可用）
        printStackTrace();

        // 触发断言以便调试器捕获
        assert(condition);
    }
}

bool CrashSafeDebug::checkWithError(bool condition, const char* message)
{
    if (!condition)
    {
        errorFlush(message);
        return false;
    }
    return true;
}

void CrashSafeDebug::crashHandler(int sig)
{
    const char* signal_name = "UNKNOWN";
    switch (sig)
    {
        case SIGSEGV: signal_name = "SIGSEGV (段错误)"; break;
        case SIGABRT: signal_name = "SIGABRT (断言失败)"; break;
        case SIGFPE:  signal_name = "SIGFPE (浮点异常)"; break;
        case SIGILL:  signal_name = "SIGILL (非法指令)"; break;
#ifndef _WIN32
        case SIGBUS:  signal_name = "SIGBUS (总线错误)"; break;
#endif
    }

    // 立即输出崩溃信息
    std::cerr << "\n=== 程序崩溃 ===" << std::endl;
    std::cerr << "信号: " << signal_name << " (" << sig << ")" << std::endl;
    std::cerr << "这通常表示程序遇到了严重错误" << std::endl;
    std::cerr.flush();

    // 输出调用栈
    std::cerr << "\n=== 调用栈 ===" << std::endl;
    printStackTrace();

    std::cerr << "\n=== 调试建议 ===" << std::endl;
    std::cerr << "1. 查看上面的错误日志了解崩溃原因" << std::endl;
    std::cerr << "2. 使用调试器 (gdb/lldb) 获取更详细信息" << std::endl;
    std::cerr << "3. 检查最近的CURA_ERROR_FLUSH输出" << std::endl;
    std::cerr.flush();

    // 恢复默认信号处理并重新触发，以便调试器捕获
    signal(sig, SIG_DFL);
    raise(sig);
}

void CrashSafeDebug::printStackTrace()
{
#if defined(__APPLE__) || defined(__linux__)
    // Unix/Linux/macOS 调用栈打印
    void* callstack[128];
    int frames = backtrace(callstack, 128);
    char** symbols = backtrace_symbols(callstack, frames);
    
    if (symbols)
    {
        for (int i = 0; i < frames; ++i)
        {
            std::string symbol = symbols[i];
            
            // 尝试解析C++符号名
            size_t start = symbol.find('(');
            size_t end = symbol.find('+');
            if (start != std::string::npos && end != std::string::npos && start < end)
            {
                std::string mangled = symbol.substr(start + 1, end - start - 1);
                int status;
                char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
                if (status == 0 && demangled)
                {
                    std::cerr << "  [" << i << "] " << demangled << std::endl;
                    free(demangled);
                }
                else
                {
                    std::cerr << "  [" << i << "] " << symbol << std::endl;
                }
            }
            else
            {
                std::cerr << "  [" << i << "] " << symbol << std::endl;
            }
        }
        free(symbols);
    }
    else
    {
        std::cerr << "无法获取调用栈信息" << std::endl;
    }
    
#elif defined(_WIN32)
    // Windows 调用栈打印
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    
    SymInitialize(process, NULL, TRUE);
    
    void* stack[100];
    unsigned short frames = CaptureStackBackTrace(0, 100, stack, NULL);
    
    SYMBOL_INFO* symbol = (SYMBOL_INFO*)calloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1);
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    
    for (unsigned int i = 0; i < frames; i++)
    {
        SymFromAddr(process, (DWORD64)(stack[i]), 0, symbol);
        std::cerr << "  [" << i << "] " << symbol->Name << " - 0x" << std::hex << symbol->Address << std::dec << std::endl;
    }
    
    free(symbol);
    SymCleanup(process);
    
#else
    std::cerr << "调用栈打印在此平台上不可用" << std::endl;
#endif
    
    std::cerr.flush();
}

} // namespace cura
