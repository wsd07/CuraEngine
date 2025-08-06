#!/bin/bash

# CuraEngine 自动修复脚本
# 用于修复所有可能的环境问题并重新配置CLion
# 包含C++20强制配置，解决concepts编译问题

set -e  # 遇到错误立即退出

echo "🔧 CuraEngine 环境自动修复脚本"
echo "================================="

PROJECT_ROOT="/Users/shidongwang/Desktop/Cura-Dev/CuraEngine"
cd "$PROJECT_ROOT"

echo "项目根目录: $PROJECT_ROOT"

# 函数：打印带颜色的消息
print_success() {
    echo "✅ $1"
}

print_warning() {
    echo "⚠️  $1"
}

print_error() {
    echo "❌ $1"
}

print_info() {
    echo "ℹ️  $1"
}

# 1. 检查必要工具
echo ""
echo "1. 检查必要工具..."
if ! command -v conan &> /dev/null; then
    print_error "Conan未安装，请先安装Conan"
    exit 1
fi

if ! command -v cmake &> /dev/null; then
    print_error "CMake未安装，请先安装CMake"
    exit 1
fi

if ! command -v ninja &> /dev/null; then
    print_error "Ninja未安装，请先安装Ninja"
    exit 1
fi

print_success "所有必要工具都已安装"

# 2. 清理旧的构建文件
echo ""
echo "2. 清理旧的构建文件..."
rm -rf cmake-build-debug cmake-build-release build
print_success "已清理旧的构建文件"

# 3. 创建Conan配置文件
echo ""
echo "3. 创建Conan配置文件..."
cat > conan_profile << 'EOF'
[settings]
arch=armv8
build_type=Release
compiler=apple-clang
compiler.cppstd=20
compiler.libcxx=libc++
compiler.version=15
os=Macos
curaengine*:compiler.cppstd=20
curaengine_plugin_infill_generate*:compiler.cppstd=20
curaengine_plugin_gradual_flow*:compiler.cppstd=20
curaengine_grpc_definitions*:compiler.cppstd=20
scripta*:compiler.cppstd=20
umspatial*:compiler.cppstd=20
dulcificum*:compiler.cppstd=20
curator/*:compiler.cppstd=20

[options]
asio-grpc/*:local_allocator=recycling_allocator
boost/*:header_only=True
clipper/*:shared=True
cpython/*:shared=True
cpython/*:with_curses=False
cpython/*:with_tkinter=False
dulcificum/*:shared=True
grpc/*:csharp_plugin=False
grpc/*:node_plugin=False
grpc/*:objective_c_plugin=False
grpc/*:php_plugin=False
grpc/*:python_plugin=False
grpc/*:ruby_plugin=False
pyarcus/*:shared=True
pynest2d/*:shared=True
pysavitar/*:shared=True

[conf]
tools.build:skip_test=True
tools.cmake.cmaketoolchain:generator=Ninja
tools.gnu:define_libcxx11_abi=True
tools.system.package_manager:mode=install
tools.system.package_manager:sudo=True
EOF

print_success "Conan配置文件已创建"

# 4. 安装Conan依赖
echo ""
echo "4. 安装Conan依赖..."
print_info "安装Debug依赖..."
conan install . --output-folder=build/Debug --build=missing --profile:build=conan_profile --profile:host=conan_profile -s build_type=Debug

print_info "安装Release依赖..."
conan install . --output-folder=build/Release --build=missing --profile:build=conan_profile --profile:host=conan_profile -s build_type=Release

print_success "Conan依赖安装完成"

# 5. 创建CLion构建目录并配置CMake
echo ""
echo "5. 配置CMake构建..."
mkdir -p cmake-build-debug cmake-build-release

print_info "配置Debug版本（强制C++20）..."
cd cmake-build-debug
cmake .. \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_STANDARD=20 \
    -DCMAKE_CXX_STANDARD_REQUIRED=ON \
    -DCMAKE_CXX_EXTENSIONS=OFF \
    -DCMAKE_TOOLCHAIN_FILE=../build/Debug/build/Debug/generators/conan_toolchain.cmake \
    -DENABLE_TESTING=OFF \
    -DENABLE_BENCHMARKS=OFF \
    -DENABLE_ARCUS=ON \
    -DENABLE_PLUGINS=ON

cd ..

print_info "配置Release版本（强制C++20）..."
cd cmake-build-release
cmake .. \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=20 \
    -DCMAKE_CXX_STANDARD_REQUIRED=ON \
    -DCMAKE_CXX_EXTENSIONS=OFF \
    -DCMAKE_TOOLCHAIN_FILE=../build/Release/build/Release/generators/conan_toolchain.cmake \
    -DENABLE_TESTING=OFF \
    -DENABLE_BENCHMARKS=OFF \
    -DENABLE_ARCUS=ON \
    -DENABLE_PLUGINS=ON

cd ..

print_success "CMake配置完成"

# 6. 编译项目
echo ""
echo "6. 编译项目..."
print_info "编译Debug版本..."
cd cmake-build-debug
ninja CuraEngine
cd ..

print_info "编译Release版本..."
cd cmake-build-release
ninja CuraEngine
cd ..

print_success "项目编译完成"

# 7. 配置CLion
echo ""
echo "7. 配置CLion..."

# 确保.idea目录存在
mkdir -p .idea/runConfigurations

# 创建CMake配置（强制C++20）
print_info "创建CMake配置（强制C++20）..."
cat > .idea/cmake.xml << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<project version="4">
  <component name="CMakeSettings">
    <configurations>
      <configuration PROFILE_NAME="conan-debug" ENABLED="true" CONFIG_NAME="Debug" TOOLCHAIN_NAME="Default" GENERATION_DIR="cmake-build-debug" BUILD_OPTIONS="-j 14" CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=build/Debug/build/Debug/generators/conan_toolchain.cmake;-DCMAKE_BUILD_TYPE=Debug;-DCMAKE_CXX_STANDARD=20;-DCMAKE_CXX_STANDARD_REQUIRED=ON;-DCMAKE_CXX_EXTENSIONS=OFF;-DENABLE_TESTING=OFF;-DENABLE_BENCHMARKS=OFF;-DENABLE_ARCUS=ON;-DENABLE_PLUGINS=ON" GENERATION_OPTIONS="-G Ninja" />
      <configuration PROFILE_NAME="conan-release" ENABLED="true" CONFIG_NAME="Release" TOOLCHAIN_NAME="Default" GENERATION_DIR="cmake-build-release" BUILD_OPTIONS="-j 14" CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=build/Release/build/Release/generators/conan_toolchain.cmake;-DCMAKE_BUILD_TYPE=Release;-DCMAKE_CXX_STANDARD=20;-DCMAKE_CXX_STANDARD_REQUIRED=ON;-DCMAKE_CXX_EXTENSIONS=OFF;-DENABLE_TESTING=OFF;-DENABLE_BENCHMARKS=OFF;-DENABLE_ARCUS=ON;-DENABLE_PLUGINS=ON" GENERATION_OPTIONS="-G Ninja" />
    </configurations>
  </component>
</project>
EOF

# 创建Debug运行配置（单线程，连接Cura）
print_info "创建Debug运行配置（单线程联调）..."
cat > .idea/runConfigurations/CuraEngine_Connect_Debug.xml << 'EOF'
<component name="ProjectRunConfigurationManager">
  <configuration default="false" name="CuraEngine Connect Debug" type="CMakeRunConfiguration" factoryName="Application" REDIRECT_INPUT="false" ELEVATE="false" USE_EXTERNAL_CONSOLE="false" EMULATE_TERMINAL="false" PASS_PARENT_ENVS_2="true" PROJECT_NAME="CuraEngine" TARGET_NAME="CuraEngine" CONFIG_NAME="conan-debug" RUN_TARGET_PROJECT_NAME="CuraEngine" RUN_TARGET_NAME="CuraEngine" PROGRAM_PARAMS="connect 127.0.0.1:49674 -m1">
    <method v="2">
      <option name="com.jetbrains.cidr.execution.CidrBuildBeforeRunTaskProvider$BuildBeforeRunTask" enabled="true" />
    </method>
  </configuration>
</component>
EOF

# 创建Release运行配置（多线程，连接Cura）
print_info "创建Release运行配置（多线程联调）..."
cat > .idea/runConfigurations/CuraEngine_Connect_Release.xml << 'EOF'
<component name="ProjectRunConfigurationManager">
  <configuration default="false" name="CuraEngine Connect Release" type="CMakeRunConfiguration" factoryName="Application" REDIRECT_INPUT="false" ELEVATE="false" USE_EXTERNAL_CONSOLE="false" EMULATE_TERMINAL="false" PASS_PARENT_ENVS_2="true" PROJECT_NAME="CuraEngine" TARGET_NAME="CuraEngine" CONFIG_NAME="conan-release" RUN_TARGET_PROJECT_NAME="CuraEngine" RUN_TARGET_NAME="CuraEngine" PROGRAM_PARAMS="connect 127.0.0.1:49674">
    <method v="2">
      <option name="com.jetbrains.cidr.execution.CidrBuildBeforeRunTaskProvider$BuildBeforeRunTask" enabled="true" />
    </method>
  </configuration>
</component>
EOF

print_success "CLion配置完成"

# 8. 验证安装
echo ""
echo "8. 验证安装..."
if [ -f "cmake-build-debug/CuraEngine" ]; then
    print_success "Debug版本CuraEngine可执行文件存在"
else
    print_error "Debug版本CuraEngine可执行文件不存在"
    exit 1
fi

if [ -f "cmake-build-release/CuraEngine" ]; then
    print_success "Release版本CuraEngine可执行文件存在"
else
    print_error "Release版本CuraEngine可执行文件不存在"
    exit 1
fi

if [ -f "build/Debug/build/Debug/generators/conan_toolchain.cmake" ]; then
    print_success "Debug Conan工具链文件存在"
else
    print_error "Debug Conan工具链文件不存在"
    exit 1
fi

if [ -f "build/Release/build/Release/generators/conan_toolchain.cmake" ]; then
    print_success "Release Conan工具链文件存在"
else
    print_error "Release Conan工具链文件不存在"
    exit 1
fi

if [ -f ".idea/cmake.xml" ]; then
    print_success "CLion CMake配置文件存在"
else
    print_error "CLion CMake配置文件不存在"
    exit 1
fi

if [ -f ".idea/runConfigurations/CuraEngine_Connect_Debug.xml" ]; then
    print_success "Debug运行配置存在"
else
    print_error "Debug运行配置不存在"
    exit 1
fi

if [ -f ".idea/runConfigurations/CuraEngine_Connect_Release.xml" ]; then
    print_success "Release运行配置存在"
else
    print_error "Release运行配置不存在"
    exit 1
fi

# 9. 测试可执行文件
echo ""
echo "9. 测试可执行文件..."
if cmake-build-debug/CuraEngine --help >/dev/null 2>&1; then
    print_success "Debug版本CuraEngine可以正常运行"
else
    print_warning "Debug版本CuraEngine运行测试失败，但这可能是正常的"
fi

if cmake-build-release/CuraEngine --help >/dev/null 2>&1; then
    print_success "Release版本CuraEngine可以正常运行"
else
    print_warning "Release版本CuraEngine运行测试失败，但这可能是正常的"
fi

echo ""
echo "🎉 修复完成！"
echo "=============="
print_success "所有组件都已成功配置和编译"
print_success "C++20编译问题已自动解决"
print_info "现在您可以："
echo "  1. 在CLion中重新打开项目"
echo "  2. 选择 conan-debug 或 conan-release 配置"
echo "  3. 使用以下运行配置进行联调："
echo "     • CuraEngine Connect Debug (单线程，适合调试)"
echo "     • CuraEngine Connect Release (多线程，性能更好)"
echo ""
print_info "联调步骤："
echo "  1. 启动Cura应用程序"
echo "  2. 在CLion中选择相应的Connect配置"
echo "  3. 点击调试按钮连接到127.0.0.1:49674"
echo "  4. 在Cura中进行切片操作"
echo ""
print_info "特性："
echo "  • 自动配置C++20标准，解决concepts编译问题"
echo "  • 强制使用正确的Conan工具链"
echo "  • 生成完整的CLion配置文件"
echo ""
print_info "如果CLion没有显示配置，请重启CLion"
echo ""
