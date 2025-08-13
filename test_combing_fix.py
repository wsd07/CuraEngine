#!/usr/bin/env python3
"""
测试脚本：验证Comb::calc()函数中边界计算一致性修复

这个脚本测试当retraction_combing=all时，retraction_combing_offset参数
是否在整个CuraEngine的相关代码中得到一致应用。
"""

import subprocess
import sys
import os
import tempfile

def create_test_stl():
    """创建一个简单的测试STL文件"""
    stl_content = """solid test
  facet normal 0 0 1
    outer loop
      vertex 0 0 0
      vertex 10 0 0
      vertex 5 10 0
    endloop
  endfacet
  facet normal 0 0 -1
    outer loop
      vertex 0 0 5
      vertex 5 10 5
      vertex 10 0 5
    endloop
  endfacet
  facet normal 0 -1 0
    outer loop
      vertex 0 0 0
      vertex 0 0 5
      vertex 10 0 5
    endloop
  endfacet
  facet normal 0 -1 0
    outer loop
      vertex 0 0 0
      vertex 10 0 5
      vertex 10 0 0
    endloop
  endfacet
  facet normal 1 0 0
    outer loop
      vertex 10 0 0
      vertex 10 0 5
      vertex 5 10 5
    endloop
  endfacet
  facet normal 1 0 0
    outer loop
      vertex 10 0 0
      vertex 5 10 5
      vertex 5 10 0
    endloop
  endfacet
  facet normal -1 1 0
    outer loop
      vertex 0 0 0
      vertex 5 10 0
      vertex 5 10 5
    endloop
  endfacet
  facet normal -1 1 0
    outer loop
      vertex 0 0 0
      vertex 5 10 5
      vertex 0 0 5
    endloop
  endfacet
endsolid test"""
    
    with tempfile.NamedTemporaryFile(mode='w', suffix='.stl', delete=False) as f:
        f.write(stl_content)
        return f.name

def create_test_settings():
    """创建测试设置，启用retraction_combing=all和设置combing_offset"""
    settings = {
        "layer_height": "0.2",
        "wall_thickness": "1.2",
        "top_thickness": "0.8",
        "bottom_thickness": "0.8",
        "fill_density": "20",
        "print_speed": "50",
        "print_temperature": "200",
        "bed_temperature": "60",
        "support_enable": "false",
        "retraction_enable": "true",
        "retraction_amount": "5",
        "retraction_speed": "25",
        "retraction_combing": "all",  # 关键设置：启用all模式
        "retraction_combing_offset": "0.5",  # 关键设置：设置combing偏移
        "travel_avoid_other_parts": "true",
        "travel_avoid_distance": "0.625"
    }
    return settings

def run_curaengine_test(stl_file, settings):
    """运行CuraEngine测试"""
    curaengine_path = "./build/Release/CuraEngine"
    
    if not os.path.exists(curaengine_path):
        print(f"错误：找不到CuraEngine可执行文件：{curaengine_path}")
        return False
    
    # 构建命令行参数
    cmd = [curaengine_path, "slice"]
    
    # 添加设置参数
    for key, value in settings.items():
        cmd.extend(["-s", f"{key}={value}"])
    
    # 启用详细输出
    cmd.append("-v")
    
    # 添加输出文件
    with tempfile.NamedTemporaryFile(suffix='.gcode', delete=False) as f:
        output_file = f.name
    
    cmd.extend(["-o", output_file])
    cmd.extend(["-l", stl_file])
    
    print(f"运行命令：{' '.join(cmd)}")
    
    try:
        # 运行CuraEngine
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        
        print(f"返回码：{result.returncode}")
        
        if result.stdout:
            print("标准输出：")
            print(result.stdout)
        
        if result.stderr:
            print("标准错误：")
            print(result.stderr)
        
        # 检查是否有崩溃或错误
        if result.returncode != 0:
            print("❌ CuraEngine执行失败")
            return False
        
        # 检查是否有我们的调试信息
        debug_output = result.stderr
        if "CombingMode::ALL detected" in debug_output:
            print("✅ 检测到CombingMode::ALL调试信息")
        
        if "extractWall0Polygons returned" in debug_output:
            print("✅ 检测到extractWall0Polygons调试信息")
        
        if "end_crossing.dest_part_ is empty" in debug_output:
            print("❌ 检测到边界计算不一致错误")
            return False
        
        # 检查输出文件是否生成
        if os.path.exists(output_file) and os.path.getsize(output_file) > 0:
            print("✅ G代码文件生成成功")
            os.unlink(output_file)  # 清理临时文件
            return True
        else:
            print("❌ G代码文件生成失败")
            return False
            
    except subprocess.TimeoutExpired:
        print("❌ CuraEngine执行超时")
        return False
    except Exception as e:
        print(f"❌ 执行过程中出现异常：{e}")
        return False
    finally:
        # 清理临时文件
        if os.path.exists(output_file):
            os.unlink(output_file)

def main():
    """主函数"""
    print("🔧 开始测试Comb::calc()边界计算一致性修复...")
    
    # 创建测试STL文件
    print("📁 创建测试STL文件...")
    stl_file = create_test_stl()
    
    try:
        # 创建测试设置
        print("⚙️ 创建测试设置...")
        settings = create_test_settings()
        
        # 运行测试
        print("🚀 运行CuraEngine测试...")
        success = run_curaengine_test(stl_file, settings)
        
        if success:
            print("✅ 测试通过！边界计算一致性修复有效。")
            return 0
        else:
            print("❌ 测试失败！可能仍存在边界计算不一致问题。")
            return 1
            
    finally:
        # 清理临时文件
        if os.path.exists(stl_file):
            os.unlink(stl_file)

if __name__ == "__main__":
    sys.exit(main())
