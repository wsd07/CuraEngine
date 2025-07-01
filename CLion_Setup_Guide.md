# CLion 配置指南 - CuraEngine

## 概述
本指南将帮助你在CLion中配置CuraEngine项目，实现编译、调试和运行功能。

✅ **配置已验证成功！**
- Debug和Release构建均已测试通过
- 可执行文件运行正常
- CLion配置文件已优化

## 前置要求

### 1. 安装依赖
确保你已经安装了以下工具：
```bash
# 安装 Conan 包管理器
pip install conan

# 安装 CMake (如果还没有)
brew install cmake

# 安装 Ninja 构建系统
brew install ninja
```

### 2. 配置 Conan Profile
```bash
# 检测并创建默认profile
conan profile detect --force
```

## CLion 配置步骤

### 1. 打开项目
1. 启动CLion
2. 选择 "Open" 并导航到CuraEngine项目根目录
3. CLion会自动检测CMakeLists.txt文件

### 2. CMake配置
CLion应该自动识别我们创建的CMake配置。如果没有，请按以下步骤操作：

1. 打开 `File` → `Settings` → `Build, Execution, Deployment` → `CMake`
2. 你应该看到两个配置：
   - **conan-debug**: Debug构建配置
   - **conan-release**: Release构建配置

### 3. 构建项目

#### 方法1: 使用构建脚本（推荐）
```bash
# Debug构建
./build_debug.sh

# Release构建
./build_release.sh
```

#### 方法2: 在CLion中构建
1. 选择构建配置（Debug或Release）
2. 点击 `Build` → `Build Project` 或使用快捷键 `Cmd+F9`

### 4. 运行配置
项目已经预配置了以下运行配置：

1. **CuraEngine Debug**: Debug模式运行CuraEngine
2. **CuraEngine Release**: Release模式运行CuraEngine  
3. **Tests Debug**: 运行单元测试

### 5. 调试配置
1. 选择 "CuraEngine Debug" 运行配置
2. 设置断点
3. 点击调试按钮或使用快捷键 `Ctrl+D`

## 项目结构

```
CuraEngine/
├── src/                    # 源代码
├── include/               # 头文件
├── tests/                 # 测试文件
├── build/                 # 构建输出
│   ├── Debug/            # Debug构建
│   └── Release/          # Release构建
├── .idea/                # CLion配置文件
└── CMakeLists.txt        # CMake配置
```

## 常用命令

### 运行CuraEngine
```bash
cd build/Debug  # 或 build/Release
./CuraEngine slice -v -p -j ../../tests/test_default_settings.txt -g ../../tests/test_global_settings.txt -l ../../tests/testModel.stl -o output.gcode
```

### 运行测试
```bash
cd build/Debug
./tests
```

## 故障排除

### 1. 依赖问题
如果遇到依赖问题，尝试重新安装Conan依赖：
```bash
rm -rf build/
./build_debug.sh
```

### 2. CMake配置问题
如果CMake配置有问题：
1. 删除 `build/` 目录
2. 在CLion中选择 `Tools` → `CMake` → `Reset Cache and Reload Project`

### 3. 库路径问题
确保环境变量正确设置：
```bash
export DYLD_LIBRARY_PATH=$PWD/build/Debug:$DYLD_LIBRARY_PATH
```

## 开发建议

1. **使用Debug配置进行开发**: 包含调试信息，便于调试
2. **定期运行测试**: 确保代码质量
3. **使用CLion的代码分析工具**: 帮助发现潜在问题
4. **配置代码格式化**: 保持代码风格一致

## 有用的CLion功能

1. **代码导航**: `Cmd+Click` 跳转到定义
2. **查找用法**: `Alt+F7` 查找符号使用
3. **重构**: `Shift+F6` 重命名符号
4. **调试器**: 设置断点、查看变量、调用栈
5. **版本控制**: 集成Git支持

## 联系支持
如果遇到问题，请参考：
- [CuraEngine官方文档](https://github.com/Ultimaker/CuraEngine/wiki)
- [CLion官方文档](https://www.jetbrains.com/help/clion/)
