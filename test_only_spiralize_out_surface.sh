#!/bin/bash

# 测试only_spiralize_out_surface功能

echo "=== 测试only_spiralize_out_surface功能 ==="

cd build/Release

# 测试1: 启用only_spiralize_out_surface功能
echo "测试1: 启用only_spiralize_out_surface功能"
echo "在spiralize模式下只保留最外层多边形"

./CuraEngine slice -v \
-s initial_extruder_nr="0" \
-s layer_height="0.3" \
-s wall_thickness="0.8" \
-s top_thickness="0" \
-s bottom_thickness="0.9" \
-s fill_density="0" \
-s print_speed="50" \
-s material_print_temperature="200" \
-s material_bed_temperature="60" \
-s machine_nozzle_size="0.4" \
-s adhesion_type="none" \
-s magic_spiralize="True" \
-s initial_bottom_layers="3" \
-s only_spiralize_out_surface="True" \
-s machine_extruder_start_code="" \
-s machine_start_gcode="" \
-s machine_end_gcode="" \
-s machine_extruder_end_code="" \
-s retraction_enable="True" \
-s retraction_amount="5" \
-s retraction_speed="25" \
-s travel_speed="120" \
-s wall_line_count="1" \
-s outer_inset_first="False" \
-s z_seam_type="shortest" \
-s z_seam_corner="z_seam_corner_weighted" \
-s z_seam_x="125.0" \
-s z_seam_y="125.0" \
-s z_seam_relative="False" \
-l ../../tests/integration/resources/cube.stl \
-o test_only_out_surface_enabled.gcode 2>&1 | grep -E "(only_spiralize_out_surface|原始多边形数量|保留最外层多边形|过滤后多边形数量)" | head -10

echo ""
echo "=== 检查结果 ==="

# 检查是否有相关日志
echo "1. 检查only_spiralize_out_surface功能日志："
./CuraEngine slice -v \
-s layer_height="0.3" \
-s wall_thickness="0.8" \
-s top_thickness="0" \
-s bottom_thickness="0.9" \
-s fill_density="0" \
-s magic_spiralize="True" \
-s initial_bottom_layers="2" \
-s only_spiralize_out_surface="True" \
-s machine_extruder_start_code="" \
-s machine_start_gcode="" \
-s machine_end_gcode="" \
-s machine_extruder_end_code="" \
-l ../../tests/integration/resources/cube.stl \
-o test_debug_out_surface.gcode 2>&1 | grep -E "(only_spiralize_out_surface功能启用|原始多边形数量|保留最外层多边形)" | head -5

echo ""
echo "2. 对比测试 - 禁用only_spiralize_out_surface："
./CuraEngine slice -v \
-s layer_height="0.3" \
-s wall_thickness="0.8" \
-s top_thickness="0" \
-s bottom_thickness="0.9" \
-s fill_density="0" \
-s magic_spiralize="True" \
-s initial_bottom_layers="2" \
-s only_spiralize_out_surface="False" \
-s machine_extruder_start_code="" \
-s machine_start_gcode="" \
-s machine_end_gcode="" \
-s machine_extruder_end_code="" \
-l ../../tests/integration/resources/cube.stl \
-o test_debug_normal.gcode 2>&1 | grep -E "(only_spiralize_out_surface功能启用|原始多边形数量)" | head -5

echo ""
echo "=== 总结 ==="
echo "如果看到以下日志，说明功能正常："
echo "1. 'only_spiralize_out_surface功能启用'"
echo "2. '原始多边形数量: X'"
echo "3. '保留最外层多边形[X]: 面积=X.XXmm²'"
echo "4. '过滤后多边形数量: 1'"

# 检查文件是否生成
if [ -f "test_only_out_surface_enabled.gcode" ]; then
    echo "✅ G-code文件生成成功，大小: $(ls -lh test_only_out_surface_enabled.gcode | awk '{print $5}')"
else
    echo "❌ G-code文件生成失败"
fi

echo ""
echo "=== 功能说明 ==="
echo "only_spiralize_out_surface参数的作用："
echo "- 当启用magic_spiralize时，如果一层有多个截面多边形"
echo "- only_spiralize_out_surface=true: 只保留最外面的多边形，舍弃内部多边形"
echo "- only_spiralize_out_surface=false: 保留所有多边形（默认行为）"
echo ""
echo "这个功能特别适用于："
echo "- 有内部孔洞的模型"
echo "- 复杂截面的花瓶模式打印"
echo "- 需要简化螺旋路径的场景"
