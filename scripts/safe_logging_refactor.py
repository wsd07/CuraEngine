#!/usr/bin/env python3
"""
CuraEngine安全日志系统重构脚本

特点：
1. 逐文件处理，每个文件都有详细记录
2. 完整的撤销功能
3. 详细的修改日志
4. 安全的备份机制
5. 可以中断和恢复
"""

import os
import re
import sys
import json
import shutil
import hashlib
from pathlib import Path
from datetime import datetime
from typing import Dict, List, Tuple, Optional

class SafeLoggingRefactor:
    def __init__(self, cura_root: str):
        self.cura_root = Path(cura_root)
        self.log_file = self.cura_root / "refactor_log.json"
        self.backup_dir = self.cura_root / "refactor_backups"
        self.backup_dir.mkdir(exist_ok=True)
        
        # 文件到分类的映射
        self.file_to_category = {
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
        
        # 调试关键词
        self.debug_keywords = [
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
        
        # 信息关键词
        self.info_keywords = [
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

    def load_log(self) -> Dict:
        """加载修改日志"""
        if self.log_file.exists():
            with open(self.log_file, 'r', encoding='utf-8') as f:
                return json.load(f)
        return {
            "version": "1.0",
            "start_time": datetime.now().isoformat(),
            "modifications": [],
            "completed_files": [],
            "failed_files": []
        }

    def save_log(self, log_data: Dict):
        """保存修改日志"""
        with open(self.log_file, 'w', encoding='utf-8') as f:
            json.dump(log_data, f, indent=2, ensure_ascii=False)

    def get_file_hash(self, filepath: str) -> str:
        """获取文件的MD5哈希"""
        with open(filepath, 'rb') as f:
            return hashlib.md5(f.read()).hexdigest()

    def backup_file(self, filepath: str) -> str:
        """备份文件，返回备份路径"""
        rel_path = Path(filepath).relative_to(self.cura_root)
        backup_path = self.backup_dir / rel_path
        backup_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(filepath, backup_path)
        return str(backup_path)

    def get_file_category(self, filepath: str) -> str:
        """根据文件路径确定调试分类"""
        filename = Path(filepath).stem
        
        # 直接匹配
        if filename in self.file_to_category:
            return self.file_to_category[filename]
        
        # 部分匹配
        for key, category in self.file_to_category.items():
            if key.lower() in filename.lower():
                return category
        
        # 默认分类
        return 'DEVELOPMENT'

    def is_debug_message(self, message: str) -> bool:
        """判断是否为调试信息"""
        message_lower = message.lower()
        
        debug_score = sum(1 for keyword in self.debug_keywords if keyword.lower() in message_lower)
        info_score = sum(1 for keyword in self.info_keywords if keyword.lower() in message_lower)
        
        return debug_score > info_score

    def refactor_spdlog_call(self, line: str, category: str) -> Tuple[str, Optional[Dict]]:
        """重构单行spdlog调用，返回新行和修改记录"""
        original_line = line
        
        # 匹配spdlog调用
        pattern = r'spdlog::(debug|info|warn|error|critical)\s*\('
        match = re.search(pattern, line)
        
        if not match:
            return line, None
        
        level = match.group(1)
        
        # 提取消息内容
        message_start = match.end()
        quote_match = re.search(r'"([^"]*)"', line[message_start:])
        message = quote_match.group(1) if quote_match else ""
        
        # 决定新的调用方式
        if level == 'debug':
            # 只有debug调用转换为分类调试
            new_line = re.sub(pattern, f'CURA_DEBUG({category}, ', line)
        else:
            # 其他级别保持原样，不做修改
            return line, None
        
        if new_line != original_line:
            return new_line, {
                "type": "spdlog_replacement",
                "original": original_line.strip(),
                "new": new_line.strip(),
                "level": level,
                "category": category,
                "message": message
            }
        
        return line, None

    def add_debug_include(self, content: str) -> Tuple[str, Optional[Dict]]:
        """添加DebugManager头文件包含"""
        if '#include "utils/DebugManager.h"' in content:
            return content, None
        
        lines = content.split('\n')
        last_include_idx = -1
        
        for i, line in enumerate(lines):
            if line.strip().startswith('#include'):
                last_include_idx = i
        
        if last_include_idx >= 0:
            lines.insert(last_include_idx + 1, '#include "utils/DebugManager.h"')
            new_content = '\n'.join(lines)
            return new_content, {
                "type": "include_addition",
                "line_number": last_include_idx + 2,
                "content": '#include "utils/DebugManager.h"'
            }
        
        return content, None

    def refactor_file(self, filepath: str) -> bool:
        """重构单个文件"""
        try:
            print(f"处理文件: {filepath}")

            # 检查文件是否存在
            if not os.path.exists(filepath):
                print(f"  ❌ 文件不存在")
                return False

            # 跳过DebugManager.h文件，避免破坏宏定义
            if 'DebugManager.h' in filepath:
                print(f"  ⏭️  跳过DebugManager.h，避免破坏宏定义")
                return False

            # 读取文件内容
            with open(filepath, 'r', encoding='utf-8') as f:
                original_content = f.read()

            # 检查是否包含spdlog调用
            if 'spdlog::' not in original_content:
                print(f"  ⏭️  无spdlog调用，跳过")
                return False
            
            # 备份文件
            backup_path = self.backup_file(filepath)
            original_hash = self.get_file_hash(filepath)
            
            # 确定文件的调试分类
            category = self.get_file_category(filepath)
            print(f"  分类: {category}")
            
            # 处理文件内容
            lines = original_content.split('\n')
            modifications = []
            modified = False
            
            # 逐行处理spdlog调用
            for i, line in enumerate(lines):
                if 'spdlog::' in line:
                    new_line, mod_record = self.refactor_spdlog_call(line, category)
                    if mod_record:
                        mod_record["line_number"] = i + 1
                        modifications.append(mod_record)
                        lines[i] = new_line
                        modified = True
                        print(f"    第{i+1}行: {mod_record['level']} -> {mod_record['type']}")
            
            if modified:
                # 添加头文件包含
                new_content = '\n'.join(lines)
                new_content, include_mod = self.add_debug_include(new_content)
                if include_mod:
                    modifications.append(include_mod)
                
                # 写回文件
                with open(filepath, 'w', encoding='utf-8') as f:
                    f.write(new_content)
                
                new_hash = self.get_file_hash(filepath)
                
                # 记录修改
                file_record = {
                    "filepath": str(Path(filepath).relative_to(self.cura_root)),
                    "timestamp": datetime.now().isoformat(),
                    "category": category,
                    "original_hash": original_hash,
                    "new_hash": new_hash,
                    "backup_path": str(Path(backup_path).relative_to(self.cura_root)),
                    "modifications": modifications,
                    "modification_count": len(modifications)
                }
                
                print(f"  ✅ 完成，{len(modifications)}处修改")
                return file_record
            else:
                print(f"  ⏭️  无需修改")
                return False
                
        except Exception as e:
            print(f"  ❌ 错误: {e}")
            return False

    def process_files(self, file_patterns: List[str] = None):
        """处理文件"""
        log_data = self.load_log()
        
        if file_patterns is None:
            # 查找所有C++文件
            cpp_files = []
            for pattern in ['**/*.cpp', '**/*.h']:
                cpp_files.extend(self.cura_root.glob(pattern))
            
            # 排除某些目录
            exclude_dirs = {'build', 'cmake-build-debug', '.git', 'stress_benchmark', 'refactor_backups'}
            cpp_files = [f for f in cpp_files if not any(part in exclude_dirs for part in f.parts)]
        else:
            cpp_files = [self.cura_root / pattern for pattern in file_patterns]
        
        print(f"找到 {len(cpp_files)} 个C++文件")
        
        processed_count = 0
        for filepath in cpp_files:
            result = self.refactor_file(str(filepath))
            if result:
                log_data["modifications"].append(result)
                log_data["completed_files"].append(str(Path(filepath).relative_to(self.cura_root)))
                processed_count += 1
            
            # 每处理10个文件保存一次日志
            if processed_count % 10 == 0:
                self.save_log(log_data)
        
        # 最终保存日志
        log_data["end_time"] = datetime.now().isoformat()
        log_data["total_processed"] = processed_count
        self.save_log(log_data)
        
        print(f"\n总结: 成功处理了 {processed_count} 个文件")
        print(f"日志文件: {self.log_file}")

    def undo_modifications(self):
        """撤销所有修改"""
        if not self.log_file.exists():
            print("没有找到修改日志文件")
            return
        
        log_data = self.load_log()
        modifications = log_data.get("modifications", [])
        
        if not modifications:
            print("没有找到需要撤销的修改")
            return
        
        print(f"准备撤销 {len(modifications)} 个文件的修改...")
        
        success_count = 0
        for mod in reversed(modifications):  # 逆序撤销
            try:
                filepath = self.cura_root / mod["filepath"]
                backup_path = self.cura_root / mod["backup_path"]
                
                if backup_path.exists():
                    shutil.copy2(backup_path, filepath)
                    print(f"✅ 撤销: {mod['filepath']}")
                    success_count += 1
                else:
                    print(f"❌ 备份文件不存在: {mod['backup_path']}")
            except Exception as e:
                print(f"❌ 撤销失败 {mod['filepath']}: {e}")
        
        if success_count == len(modifications):
            # 删除日志文件和备份目录
            self.log_file.unlink()
            shutil.rmtree(self.backup_dir)
            print(f"✅ 成功撤销所有 {success_count} 个修改")
        else:
            print(f"⚠️  部分撤销成功: {success_count}/{len(modifications)}")

    def show_status(self):
        """显示当前状态"""
        if not self.log_file.exists():
            print("没有进行中的重构任务")
            return
        
        log_data = self.load_log()
        modifications = log_data.get("modifications", [])
        
        print(f"重构状态:")
        print(f"  开始时间: {log_data.get('start_time', 'Unknown')}")
        print(f"  已处理文件: {len(modifications)}")
        print(f"  总修改数: {sum(mod.get('modification_count', 0) for mod in modifications)}")
        
        if modifications:
            print(f"\n最近处理的文件:")
            for mod in modifications[-5:]:
                print(f"  {mod['filepath']} ({mod['modification_count']}处修改)")

def main():
    if len(sys.argv) < 2:
        print("用法:")
        print("  python3 safe_logging_refactor.py process [file1] [file2] ...")
        print("  python3 safe_logging_refactor.py undo")
        print("  python3 safe_logging_refactor.py status")
        return
    
    cura_root = Path(__file__).parent.parent
    refactor = SafeLoggingRefactor(str(cura_root))
    
    command = sys.argv[1]
    
    if command == "process":
        file_patterns = sys.argv[2:] if len(sys.argv) > 2 else None
        refactor.process_files(file_patterns)
    elif command == "undo":
        refactor.undo_modifications()
    elif command == "status":
        refactor.show_status()
    else:
        print(f"未知命令: {command}")

if __name__ == '__main__':
    main()
