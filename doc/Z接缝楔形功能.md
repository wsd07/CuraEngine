# Z接缝楔形功能

## 概述

Z接缝楔形功能通过在外墙起始和结束处创建楔形重叠来消除接缝瑕疵。该功能在外墙的起始点和终止点之间形成楔形交叠，从而实现无缝连接。

## 功能原理

### 楔形结构

外墙打印过程分为三个阶段：

1. **起始楔形**：从起始点开始，喷头Z高度从前一层高度线性增加到当前层高度，流量从0线性增加到1
2. **正常打印**：Z高度保持当前层高度，流量保持1
3. **结束楔形**：在结束处，Z高度保持当前层高度，流量从1线性减少到0

### 数学模型

```
起始楔形阶段 (0 ≤ distance ≤ wedge_length):
- Z坐标: Z_prev + (distance / wedge_length) × layer_thickness
- 流量: distance / wedge_length

正常打印阶段 (wedge_length < distance < wall_length - wedge_length):
- Z坐标: Z_current
- 流量: 1.0

结束楔形阶段 (wall_length - wedge_length ≤ distance ≤ wall_length):
- Z坐标: Z_current
- 流量: 1.0 - (distance - (wall_length - wedge_length)) / wedge_length
```

## 参数配置

### 新增参数

#### `z_seam_wedge_length`
- **类型**: 浮点数
- **单位**: mm
- **范围**: 0.0 - 10.0
- **默认值**: 0.0
- **描述**: 楔形重叠的长度

### 启用条件

楔形功能仅在以下条件下启用：

1. `z_seam_wedge_length > 0`
2. `is_closed = true`（闭合轮廓）
3. `layer_nr > 0`（非第一层）
4. 满足以下模式条件之一：
   - **非螺旋模式**: `magic_spiralize = false`
   - **螺旋模式但无Z渐进**: `magic_spiralize = true` 且 `smooth_spiralized_z = false`
   - **螺旋模式的非螺旋层**: `magic_spiralize = true` 且当前层为非螺旋层

### 冲突检测

楔形功能会自动检测并避免与现有Z渐进功能冲突：

- 与 `smooth_spiralized_z` 冲突时自动禁用
- 与螺旋模式的第一层和最后一层Z渐进冲突时自动禁用

## 技术实现

### 核心修改位置

**文件**: `CuraEngine/src/LayerPlan.cpp`
**函数**: `addSplitWall`

### 关键算法

#### 1. 条件判断
```cpp
bool enable_z_seam_wedge = false;
if (z_seam_wedge_length > 0 && is_closed && layer_nr_ > 0) {
    const bool magic_spiralize = Application::getInstance().current_slice_->scene.current_mesh_group->settings.get<bool>("magic_spiralize");
    
    if (!magic_spiralize) {
        // 非螺旋模式：总是启用
        enable_z_seam_wedge = true;
    } else {
        // 螺旋模式：需要进一步检查
        const bool smooth_spiralized_z = Application::getInstance().current_slice_->scene.current_mesh_group->settings.get<bool>("smooth_spiralized_z");
        
        if (!smooth_spiralized_z) {
            // 检查是否是螺旋层
            const bool is_spiralize_layer = layer_nr_ >= LayerIndex(initial_bottom_layers);
            enable_z_seam_wedge = !is_spiralize_layer;
        }
    }
}
```

#### 2. 楔形处理
```cpp
// 起始楔形
if (process_z_wedge_start) {
    const double wedge_start_factor = static_cast<double>(destination_position) / static_cast<double>(effective_wedge_length);
    split_destination.z_ = static_cast<coord_t>(-layer_thickness_ + wedge_start_factor * layer_thickness_);
    z_wedge_flow_ratio = wedge_start_factor;
}

// 结束楔形
if (process_z_wedge_end) {
    const double wedge_end_factor = 1.0 - static_cast<double>(destination_position - wedge_end_start) / static_cast<double>(effective_wedge_length);
    split_destination.z_ = 0;
    z_wedge_flow_ratio = wedge_end_factor;
}
```

#### 3. 流量应用
```cpp
func_add_segment(
    split_origin,
    split_destination,
    accelerate_speed_factor * decelerate_speed_factor,
    flow_ratio * scarf_segment_flow_ratio * z_wedge_flow_ratio,  // 应用楔形流量
    line_width_ratio,
    distance_to_bridge_start);
```

### 分段策略

- **分段距离**: 0.1mm
- **最大楔形长度**: 不超过墙体长度的一半
- **Z坐标精度**: 微米级别

## 使用示例

### 基础配置
```ini
# 启用Z接缝楔形
z_seam_wedge_length = 2.0

# 非螺旋模式
magic_spiralize = false
```

### 螺旋模式配置
```ini
# 启用Z接缝楔形
z_seam_wedge_length = 1.5

# 螺旋模式但禁用Z渐进
magic_spiralize = true
smooth_spiralized_z = false
```

## 效果预期

### 打印质量改善

1. **消除接缝线**: 楔形重叠消除了明显的接缝线
2. **平滑过渡**: 渐变的Z高度和流量实现平滑过渡
3. **减少瑕疵**: 避免了起始和结束点的挤出不均

### 适用场景

- **外观要求高的打印件**
- **圆形或曲线轮廓**
- **薄壁结构**
- **装饰性物品**

## 兼容性

### 支持的模式
- ✅ 常规打印模式
- ✅ 螺旋模式（非Z渐进）
- ✅ 螺旋模式的非螺旋层

### 不支持的模式
- ❌ 螺旋模式 + Z渐进 (`smooth_spiralized_z = true`)
- ❌ 开放轮廓 (`is_closed = false`)
- ❌ 第一层 (`layer_nr = 0`)

## 调试信息

功能运行时会输出详细的调试信息：

```
【Z接缝楔形】第5层，启用楔形功能，楔形长度：2.0mm
【Z接缝楔形】起始楔形，进度：45%，Z偏移：-0.18mm，流量：0.45
【Z接缝楔形】结束楔形，进度：80%，Z偏移：0.00mm，流量：0.20
```

## 性能影响

- **计算开销**: 轻微增加（额外的插值计算）
- **内存使用**: 无显著影响
- **打印时间**: 无影响（相同的路径长度）
- **G代码大小**: 轻微增加（更多的分段）
