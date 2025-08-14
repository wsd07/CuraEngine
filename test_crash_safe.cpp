#include "utils/CrashSafeDebug.h"
#include <iostream>

int main() {
    std::cout << "测试崩溃安全调试系统..." << std::endl;
    
    // 初始化崩溃安全系统
    cura::CrashSafeDebug::initialize();
    
    // 测试错误日志
    CURA_ERROR_FLUSH("这是一个测试错误消息");
    CURA_ERROR_FLUSH_F("格式化测试: %d + %d = %d", 1, 2, 3);
    
    // 测试条件检查
    bool test_condition = false;
    if (!CURA_CHECK_WITH_ERROR(test_condition, "条件检查失败，但程序继续运行")) {
        std::cout << "条件检查失败，但程序没有崩溃" << std::endl;
    }
    
    std::cout << "是否要测试断言崩溃？(y/n): ";
    char choice;
    std::cin >> choice;
    
    if (choice == 'y' || choice == 'Y') {
        std::cout << "即将触发断言崩溃..." << std::endl;
        CURA_ASSERT_WITH_INFO(false, "这是一个测试断言失败，程序将崩溃并显示详细信息");
    }
    
    std::cout << "测试完成，程序正常退出" << std::endl;
    return 0;
}
