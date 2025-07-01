#!/bin/bash

# CuraEngine Release Build Script for CLion
# This script sets up the release build environment

set -e

echo "Setting up CuraEngine Release build..."

# Check if conan is installed
if ! command -v conan &> /dev/null; then
    echo "Error: Conan is not installed. Please install conan first:"
    echo "pip install conan"
    exit 1
fi

# Create build directory if it doesn't exist
mkdir -p build/Release

# Install dependencies with conan
echo "Installing dependencies with Conan..."
conan install . --output-folder=build/Release --build=missing -s build_type=Release -c tools.system.package_manager:mode=install -c tools.system.package_manager:sudo=True

# Configure CMake
echo "Configuring CMake..."
cd build/Release
cmake ../.. -DCMAKE_TOOLCHAIN_FILE=generators/conan_toolchain.cmake \
           -DCMAKE_BUILD_TYPE=Release \
           -DENABLE_TESTING=OFF \
           -DENABLE_BENCHMARKS=OFF \
           -DENABLE_ARCUS=ON \
           -DENABLE_PLUGINS=ON \
           -G Ninja

# Build the project
echo "Building CuraEngine..."
cmake --build . --config Release -j 14

echo "Release build completed successfully!"
echo "Executable location: $(pwd)/CuraEngine"
echo ""
echo "To run CuraEngine:"
echo "cd $(pwd)"
echo "./CuraEngine slice -v -p -j ../../tests/test_default_settings.txt -g ../../tests/test_global_settings.txt -l ../../tests/testModel.stl -o output.gcode"
