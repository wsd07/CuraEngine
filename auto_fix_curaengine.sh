#!/bin/bash

# CuraEngine 自动修复脚本
# 用于修复所有可能的环境问题并重新配置CLion

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
compiler.cppstd=17
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

print_info "配置Debug版本..."
cd cmake-build-debug
cmake .. \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE=../build/Debug/build/Debug/generators/conan_toolchain.cmake \
    -DENABLE_TESTING=OFF \
    -DENABLE_BENCHMARKS=OFF \
    -DENABLE_ARCUS=ON \
    -DENABLE_PLUGINS=ON

cd ..

print_info "配置Release版本..."
cd cmake-build-release
cmake .. \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
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
