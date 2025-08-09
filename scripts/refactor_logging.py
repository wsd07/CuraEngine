#!/usr/bin/env python3
"""
CuraEngine日志系统重构脚本

这个脚本会：
1. 分析现有的spdlog调用
2. 根据文件和上下文自动分类
3. 将调试信息转换为分类调试输出
4. 将非调试信息保持为info/warn/error级别
"""

import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Tuple

# 文件到调试分类的映射
FILE_TO_CATEGORY = {
    # BeadingStrategy相关
    'BeadingStrategy': 'BEADING_STRATEGY',
    'DistributedBeadingStrategy': 'BEADING_STRATEGY', 
    'RedistributeBeadingStrategy': 'BEADING_STRATEGY',
    'WideningBeadingStrategy': 'BEADING_STRATEGY',
    'LimitedBeadingStrategy': 'BEADING_STRATEGY',
    'OuterWallInsetBeadingStrategy': 'BEADING_STRATEGY',
    'FlowCompensatedBeadingStrategy': 'FLOW_COMPENSATION',
    
    # 骨架梯形化
    'SkeletalTrapezoidation': 'SKELETAL_TRAPEZOIDATION',
    'SkeletalTrapezoidationGraph': 'SKELETAL_TRAPEZOIDATION',
    
    # 墙体计算
    'WallToolPaths': 'WALL_COMPUTATION',
    'WallsComputation': 'WALL_COMPUTATION',
    
    # 填充
    'infill': 'INFILL',
    'SierpinskiFill': 'INFILL',
    'ImageBasedDensityProvider': 'INFILL',
    
    # 支撑
    'support': 'SUPPORT',
    'TreeSupport': 'TREE_SUPPORT',
    'TreeSupportTipGenerator': 'TREE_SUPPORT',
    'TreeModelVolumes': 'TREE_SUPPORT',
    'TreeSupportUtils': 'TREE_SUPPORT',
    
    # 路径规划
    'LayerPlan': 'LAYER_PLAN',
    'LayerPlanBuffer': 'LAYER_PLAN',
    'PathOrderOptimizer': 'PATH_PLANNING',
    
    # 几何处理
    'mesh': 'MESH_PROCESSING',
    'slicer': 'SLICING',
    'Slice': 'SLICING',
    'VoronoiUtils': 'GEOMETRY',
    'SVG': 'GEOMETRY',
    'ExtrusionSegment': 'GEOMETRY',
    
    # 设置
    'Settings': 'SETTINGS',
    'AdaptiveLayerHeights': 'ADAPTIVE_LAYERS',
    'HeightParameterGraph': 'SETTINGS',
    'FlowTempGraph': 'SETTINGS',
    'ZSeamConfig': 'SEAM_PLACEMENT',
    
    # G代码生成
    'FffGcodeWriter': 'GCODE_GENERATION',
    'gcodeExport': 'GCODE_EXPORT',
    
    # 通信
    'CommandLine': 'COMMUNICATION',
    'ArcusCommunication': 'COMMUNICATION',
    'EmscriptenCommunication': 'COMMUNICATION',
    'Listener': 'COMMUNICATION',
    
    # 插件
    'pluginproxy': 'PLUGINS',
    
    # 性能和进度
    'Progress': 'PROGRESS',
    'gettime': 'PERFORMANCE',
    
    # 其他
    'Application': 'DEVELOPMENT',
    'main': 'DEVELOPMENT',
    'MeshGroup': 'MESH_PROCESSING',
    'Scene': 'DEVELOPMENT',
    'Preheat': 'DEVELOPMENT',
    'PrimeTower': 'DEVELOPMENT',
    'raft': 'DEVELOPMENT',
    'SkirtBrim': 'DEVELOPMENT',
    'channel': 'COMMUNICATION',
}

# 调试关键词 - 包含这些词的通常是调试信息
DEBUG_KEYWORDS = [
    '调试', 'debug', 'Debug', 'DEBUG',
    '详细', 'verbose', 'Verbose',
    '计算', '处理', '生成', '检查', '验证',
    'compute', 'process', 'generate', 'check', 'verify',
    '开始', '结束', '完成', 'start', 'end', 'finish', 'complete',
    '参数', 'parameter', 'param', 'setting',
    '算法', 'algorithm', 'strategy',
    '几何', 'geometry', 'polygon', 'mesh',
    '路径', 'path', 'route', 'travel',
    '层', 'layer', 'slice',
    '线宽', 'width', 'bead',
    '流量', 'flow', 'extrusion',
    '补偿', 'compensation', 'adjust',
]

# 信息关键词 - 包含这些词的通常是用户信息
INFO_KEYWORDS = [
    '加载', '保存', '读取', '写入',
    'load', 'save', 'read', 'write', 'export',
    '启动', '初始化', '配置',
    'start', 'init', 'config', 'setup',
    '进度', 'progress', 'percent', '%',
    '完成', '成功', 'complete', 'success', 'done',
    '时间', 'time', 'duration', 'elapsed',
    '统计', 'statistics', 'stats', 'count',
    '模式', 'mode', 'enabled', 'disabled',
]

def get_file_category(filepath: str) -> str:
    """根据文件路径确定调试分类"""
    filename = Path(filepath).stem
    
    # 直接匹配
    if filename in FILE_TO_CATEGORY:
        return FILE_TO_CATEGORY[filename]
    
    # 部分匹配
    for key, category in FILE_TO_CATEGORY.items():
        if key.lower() in filename.lower():
            return category
    
    # 默认分类
    return 'DEVELOPMENT'

def is_debug_message(message: str) -> bool:
    """判断是否为调试信息"""
    message_lower = message.lower()
    
    # 检查是否包含调试关键词
    debug_score = sum(1 for keyword in DEBUG_KEYWORDS if keyword.lower() in message_lower)
    info_score = sum(1 for keyword in INFO_KEYWORDS if keyword.lower() in message_lower)
    
    # 如果调试关键词更多，认为是调试信息
    return debug_score > info_score

def refactor_spdlog_call(line: str, category: str) -> str:
    """重构单行spdlog调用"""
    # 匹配spdlog调用
    pattern = r'spdlog::(debug|info|warn|error|critical)\s*\('
    match = re.search(pattern, line)
    
    if not match:
        return line
    
    level = match.group(1)
    
    # 提取消息内容（简单的启发式方法）
    message_start = match.end()
    # 找到第一个字符串字面量
    quote_match = re.search(r'"([^"]*)"', line[message_start:])
    if quote_match:
        message = quote_match.group(1)
    else:
        message = ""
    
    # 决定新的调用方式
    if level == 'debug' or is_debug_message(message):
        # 转换为分类调试
        new_call = f'CURA_DEBUG({category}, '
        return re.sub(pattern, new_call, line)
    elif level == 'info':
        # 保持为info
        return re.sub(r'spdlog::info', 'CURA_INFO', line)
    elif level == 'warn':
        # 保持为warn
        return re.sub(r'spdlog::warn', 'CURA_WARN', line)
    elif level == 'error' or level == 'critical':
        # 保持为error
        return re.sub(r'spdlog::(error|critical)', 'CURA_ERROR', line)
    
    return line

def add_debug_include(content: str) -> str:
    """添加DebugManager头文件包含"""
    if '#include "utils/DebugManager.h"' in content:
        return content
    
    # 找到最后一个#include行
    lines = content.split('\n')
    last_include_idx = -1
    
    for i, line in enumerate(lines):
        if line.strip().startswith('#include'):
            last_include_idx = i
    
    if last_include_idx >= 0:
        lines.insert(last_include_idx + 1, '#include "utils/DebugManager.h"')
        return '\n'.join(lines)
    
    return content

def refactor_file(filepath: str) -> bool:
    """重构单个文件"""
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()
        
        # 检查是否包含spdlog调用
        if 'spdlog::' not in content:
            return False
        
        print(f"重构文件: {filepath}")
        
        # 确定文件的调试分类
        category = get_file_category(filepath)
        print(f"  分类: {category}")
        
        lines = content.split('\n')
        modified = False
        
        for i, line in enumerate(lines):
            if 'spdlog::' in line:
                new_line = refactor_spdlog_call(line, category)
                if new_line != line:
                    lines[i] = new_line
                    modified = True
                    print(f"  修改: {line.strip()} -> {new_line.strip()}")
        
        if modified:
            # 添加头文件包含
            new_content = '\n'.join(lines)
            new_content = add_debug_include(new_content)
            
            # 写回文件
            with open(filepath, 'w', encoding='utf-8') as f:
                f.write(new_content)
            
            print(f"  ✅ 已更新")
            return True
        else:
            print(f"  ⏭️  无需修改")
            return False
            
    except Exception as e:
        print(f"  ❌ 错误: {e}")
        return False

def main():
    """主函数"""
    if len(sys.argv) > 1:
        # 处理指定文件
        for filepath in sys.argv[1:]:
            refactor_file(filepath)
    else:
        # 处理所有C++文件
        cura_root = Path(__file__).parent.parent
        cpp_files = []
        
        # 查找所有.cpp和.h文件
        for pattern in ['**/*.cpp', '**/*.h']:
            cpp_files.extend(cura_root.glob(pattern))
        
        # 排除某些目录
        exclude_dirs = {'build', 'cmake-build-debug', '.git', 'stress_benchmark'}
        cpp_files = [f for f in cpp_files if not any(part in exclude_dirs for part in f.parts)]
        
        print(f"找到 {len(cpp_files)} 个C++文件")
        
        modified_count = 0
        for filepath in cpp_files:
            if refactor_file(str(filepath)):
                modified_count += 1
        
        print(f"\n总结: 修改了 {modified_count} 个文件")

if __name__ == '__main__':
    main()
