# Retraction Combing 功能

## 概述

Retraction Combing 是 CuraEngine 中的避障移动功能，通过智能路径规划避免喷嘴在移动时与已打印结构碰撞，减少回抽次数，提高打印效率和质量。

## 核心功能

### 1. Combing 边界偏移功能

**新参数**: `retraction_combing_offset`

#### 参数定义

```json
"retraction_combing_offset": {
    "label": "Combing Boundary Offset",
    "description": "When combing mode is set to 'All', this offset is applied to the outer wall (wall_0) polygons to define the combing boundary. Positive values expand the boundary outward (allowing travel further from walls), negative values shrink it inward (keeping travel closer to walls). This provides precise control over where the nozzle can travel during combing moves.",
    "unit": "mm",
    "type": "float",
    "default_value": 0,
    "minimum_value": "-10",
    "maximum_value": "10",
    "enabled": "resolveOrValue('retraction_combing') == 'all'",
    "settable_per_mesh": false,
    "settable_per_extruder": true
}
```

#### 实现逻辑

**文件位置**: `CuraEngine/src/LayerPlan.cpp`

```cpp
// 对于 CombingMode::ALL，使用外墙多边形而不是 part.outline
if (combing_mode == CombingMode::ALL)
{
    // 获取 retraction_combing_offset 参数
    coord_t combing_offset = mesh.settings.get<coord_t>("retraction_combing_offset");
    
    // 从 wall_toolpaths 中提取外墙（inset_idx == 0）的多边形
    Shape wall0_polygons = extractWall0Polygons(part);
    
    if (!wall0_polygons.empty())
    {
        // 使用外墙多边形并应用 combing_offset
        part_combing_boundary = wall0_polygons.offset(combing_offset + offset);
    }
    else
    {
        // 回退到使用 part.outline（向后兼容）
        part_combing_boundary = part.outline.offset(offset);
    }
}
```

#### 技术特点

1. **精确边界控制**: 基于实际外墙多边形而不是轮廓近似
2. **可调偏移**: 支持 -10mm 到 +10mm 的精确偏移
3. **向后兼容**: 默认值为0，不改变现有行为
4. **条件启用**: 仅在 `retraction_combing = All` 时生效

### 2. 层间路径优化 Combing 改进

**问题**: `optimizeLayerEndForNextLayerStart` 函数中的空移路径使用直线连接，导致跨越模型造成拉丝。

**解决方案**: 将所有直线 travel 路径改为 combing travel 路径。

#### 新增成员函数

```cpp
/*!
 * \brief 创建一个使用 combing 的 travel 路径
 * \param from_point 起始点
 * \param to_point 目标点
 * \param layer_z 层高度
 * \param extruder_nr 挤出机编号
 * \return 包含 combing 路径的 GCodePath 向量
 */
std::vector<GCodePath> LayerPlan::createCombingTravel(const Point2LL& from_point, const Point2LL& to_point, coord_t layer_z, size_t extruder_nr);
```

#### 实现特点

- **智能回退**: 无 comb 对象时自动回退到直线 travel
- **边界检测**: 使用 `comb_boundary_preferred_` 检测起点和终点是否在边界内
- **完整 combing**: 调用 `comb_->calc()` 计算完整的 combing 路径
- **多段路径**: 支持生成多段 combing 路径
- **参数兼容**: 支持所有 combing 相关参数（z_hop、retraction 等）

## Combing 模式

### 1. Off 模式

- **描述**: 禁用 combing，所有移动使用直线路径
- **适用**: 简单模型或需要最快切片速度的场景

### 2. All 模式

- **描述**: 在所有区域内进行 combing
- **边界**: 使用外墙多边形 + `retraction_combing_offset` 偏移
- **适用**: 复杂模型，需要最大程度避免拉丝

### 3. No Skin 模式

- **描述**: 避开表面区域，允许在填充区域内 combing
- **边界**: 排除表面区域的内部区域
- **适用**: 表面质量要求高的模型

### 4. No Outer Surfaces 模式

- **描述**: 避开外表面，允许在内部区域 combing
- **边界**: 排除外表面的内部区域
- **适用**: 外观质量要求极高的模型

### 5. Infill 模式

- **描述**: 仅在填充区域内进行 combing
- **边界**: 仅填充区域
- **适用**: 快速打印，对表面质量要求不高

## 相关参数

### 核心参数

1. **retraction_combing**
   - 类型: 枚举
   - 选项: off, all, noskin, no_outer_surfaces, infill
   - 描述: Combing 模式选择

2. **retraction_combing_offset**
   - 类型: 浮点数
   - 单位: mm
   - 范围: -10 到 10
   - 描述: Combing 边界偏移

3. **retraction_combing_max_distance**
   - 类型: 浮点数
   - 单位: mm
   - 描述: 最大 combing 距离，超过此距离将使用回抽

### 辅助参数

1. **retraction_combing_avoid_distance**
   - 描述: 避障距离
   - 影响: combing 路径与障碍物的最小距离

2. **travel_avoid_other_parts**
   - 描述: 是否避开其他部件
   - 影响: 多部件模型的 combing 行为

## 使用场景

### 1. 精密打印

**配置**:
```ini
retraction_combing = all
retraction_combing_offset = -0.2  # 向内收缩，更接近外墙
```

**效果**: 限制喷嘴移动更接近外墙，减少对内部结构的干扰

### 2. 高速打印

**配置**:
```ini
retraction_combing = all
retraction_combing_offset = 0.5   # 向外扩展，更大移动空间
```

**效果**: 扩展避障边界，允许更直接的移动路径，提高打印速度

### 3. 复杂几何

**配置**:
```ini
retraction_combing = all
retraction_combing_offset = 0.1   # 小正值，平衡安全和效率
```

**效果**: 在保持安全距离的同时，提供足够的移动空间

### 4. 表面质量优先

**配置**:
```ini
retraction_combing = no_outer_surfaces
retraction_combing_offset = 0     # 使用默认边界
```

**效果**: 避开外表面，保护外观质量

## 性能影响

### 1. 计算开销

- **外墙提取**: 一次性计算，缓存结果
- **多边形偏移**: 使用高效的 Clipper 库
- **路径计算**: 使用优化的 combing 算法

### 2. 内存使用

- **边界存储**: 临时存储外墙多边形
- **网格缓存**: 缓存 combing 网格
- **路径缓存**: 缓存计算结果

### 3. 优化措施

- **早期退出**: 无外墙数据时直接使用原有逻辑
- **数据复用**: 避免重复计算
- **智能缓存**: 合理的缓存策略

## 调试和监控

### 1. 日志输出

```cpp
spdlog::debug("Combing boundary offset: {:.2f}mm", INT2MM(combing_offset));
spdlog::debug("Wall0 polygons found: {}", wall0_polygons.size());
```

### 2. 性能监控

```cpp
auto start_time = std::chrono::high_resolution_clock::now();
// Combing 计算...
auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
spdlog::debug("Combing calculation took {} ms", duration.count());
```

### 3. 质量检查

- **边界有效性**: 检查生成的边界是否有效
- **路径长度**: 监控 combing 路径的长度
- **碰撞检测**: 验证路径是否真正避开障碍

## 国际化支持

### 简体中文
- **label**: "避障边界偏移"
- **description**: "当避障模式设置为"全部"时，此偏移量应用于外墙（wall_0）多边形以定义避障边界。正值向外扩展边界（允许远离墙壁移动），负值向内收缩边界（保持移动更接近墙壁）。这提供了对喷嘴在避障移动期间可以移动位置的精确控制。"

### 繁体中文
- **label**: "避障邊界偏移"
- **description**: "當避障模式設置為「全部」時，此偏移量應用於外牆（wall_0）多邊形以定義避障邊界。正值向外擴展邊界（允許遠離牆壁移動），負值向內收縮邊界（保持移動更接近牆壁）。這提供了對噴嘴在避障移動期間可以移動位置的精確控制。"

### 日语
- **label**: "コーミング境界オフセット"
- **description**: "コーミングモードが「すべて」に設定されている場合、このオフセットは外壁（wall_0）ポリゴンに適用されてコーミング境界を定義します。正の値は境界を外側に拡張し（壁からより遠くへの移動を許可）、負の値は内側に収縮させます（移動を壁により近く保つ）。これにより、コーミング移動中にノズルが移動できる場所を正確に制御できます。"

## 未来改进方向

### 1. 动态边界调整

根据打印进度动态调整 combing 边界，适应不断变化的打印环境。

### 2. 智能模式选择

基于模型特征自动选择最适合的 combing 模式。

### 3. 机器学习优化

使用机器学习技术优化 combing 路径，减少计算时间并提高路径质量。
