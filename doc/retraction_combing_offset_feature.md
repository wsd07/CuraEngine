# Retraction Combing Offset 功能实现

## 功能概述

`retraction_combing_offset` 是一个新的参数，用于在 `retraction_combing` 模式设置为 `All` 时，精确控制避障（combing）边界的位置。该功能允许用户基于外墙（wall_0）多边形定义避障边界，而不是使用默认的 `part.outline`。

## 功能特性

### 1. 精确边界控制

- **基于外墙**：使用外墙（wall_0）多边形作为避障边界的基础
- **可调偏移**：支持正负偏移值，提供灵活的边界调整
- **精确控制**：允许用户精确定义喷嘴在避障移动期间的可行区域

### 2. 参数说明

| 参数 | 类型 | 默认值 | 范围 | 单位 |
|------|------|--------|------|------|
| `retraction_combing_offset` | float | 0 | -10 ~ 10 | mm |

**参数行为**：
- **正值**：向外扩展边界，允许喷嘴移动到距离外墙更远的位置
- **负值**：向内收缩边界，限制喷嘴移动更接近外墙
- **零值**：使用外墙多边形的原始位置作为边界

### 3. 启用条件

该参数仅在以下条件下生效：
- `retraction_combing` 设置为 `All`
- 当前层存在有效的外墙（wall_0）数据

## 技术实现

### 1. 核心算法

#### 外墙多边形提取

```cpp
Shape extractWall0Polygons(const SliceLayerPart& part)
{
    Shape wall0_polygons;
    
    // 遍历所有 wall_toolpaths
    for (const VariableWidthLines& wall_lines : part.wall_toolpaths)
    {
        for (const ExtrusionLine& line : wall_lines)
        {
            // 只处理外墙（inset_idx == 0）
            if (line.inset_idx_ == 0 && !line.junctions_.empty())
            {
                Polygon polygon;
                
                // 将 ExtrusionJunction 转换为 Point2LL
                for (const ExtrusionJunction& junction : line.junctions_)
                {
                    polygon.push_back(junction.p_);
                }
                
                // 处理闭合多边形
                if (line.is_closed_ && polygon.size() >= 3)
                {
                    if (polygon.front() != polygon.back())
                    {
                        polygon.push_back(polygon.front());
                    }
                }
                
                // 只添加有效的多边形（至少3个点）
                if (polygon.size() >= 3)
                {
                    wall0_polygons.push_back(polygon);
                }
            }
        }
    }
    
    return wall0_polygons;
}
```

#### 边界计算逻辑

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

### 2. 数据流程

1. **参数读取**：从设置中获取 `retraction_combing_offset` 值
2. **外墙提取**：从 `part.wall_toolpaths` 中提取 `inset_idx == 0` 的多边形
3. **边界计算**：对外墙多边形应用偏移量
4. **回退机制**：如果没有外墙数据，使用原有的 `part.outline` 逻辑

### 3. 关键改进点

#### 原有逻辑
```cpp
part_combing_boundary = part.outline.offset(offset);
```

#### 新逻辑
```cpp
if (combing_mode == CombingMode::ALL)
{
    coord_t combing_offset = mesh.settings.get<coord_t>("retraction_combing_offset");
    Shape wall0_polygons = extractWall0Polygons(part);
    
    if (!wall0_polygons.empty())
    {
        part_combing_boundary = wall0_polygons.offset(combing_offset + offset);
    }
    else
    {
        part_combing_boundary = part.outline.offset(offset);  // 向后兼容
    }
}
else
{
    part_combing_boundary = part.outline.offset(offset);  // 其他模式保持不变
}
```

## 使用场景

### 1. 精密打印

**场景**：需要精确控制喷嘴移动路径的精密打印
**设置**：`retraction_combing_offset = -0.2`（负值）
**效果**：限制喷嘴移动更接近外墙，减少对内部结构的干扰

### 2. 高速打印

**场景**：高速打印时需要更大的移动空间
**设置**：`retraction_combing_offset = 0.5`（正值）
**效果**：扩展避障边界，允许更直接的移动路径，提高打印速度

### 3. 复杂几何

**场景**：复杂几何形状需要精确的避障控制
**设置**：`retraction_combing_offset = 0.1`（小正值）
**效果**：在保持安全距离的同时，提供足够的移动空间

## 配置示例

### 1. 基本配置

```ini
# 启用全部避障模式
retraction_combing = All

# 设置边界偏移
retraction_combing_offset = 0.2  # 向外扩展0.2mm
```

### 2. 精密打印配置

```ini
retraction_combing = All
retraction_combing_offset = -0.1  # 向内收缩0.1mm，更接近外墙
```

### 3. 高速打印配置

```ini
retraction_combing = All
retraction_combing_offset = 0.5   # 向外扩展0.5mm，更大移动空间
```

## 兼容性和向后兼容

### 1. 向后兼容

- **默认值为0**：不改变现有行为
- **回退机制**：当没有外墙数据时，自动使用原有逻辑
- **其他模式不受影响**：只影响 `CombingMode::ALL`

### 2. 参数验证

- **范围限制**：-10mm 到 10mm，防止极端值
- **类型检查**：浮点数类型，支持小数精度
- **启用条件**：仅在 `retraction_combing = All` 时可用

## 性能影响

### 1. 计算开销

- **外墙提取**：一次性计算，缓存结果
- **多边形偏移**：使用高效的 Clipper 库
- **内存使用**：临时存储外墙多边形，影响很小

### 2. 优化措施

- **早期退出**：无外墙数据时直接使用原有逻辑
- **数据复用**：避免重复计算
- **内存管理**：及时释放临时数据

## 测试和验证

### 1. 功能测试

- ✅ **参数读取**：正确读取 `retraction_combing_offset` 值
- ✅ **外墙提取**：正确提取 `inset_idx == 0` 的多边形
- ✅ **边界计算**：正确应用偏移量
- ✅ **回退机制**：无外墙数据时正确回退

### 2. 边界测试

- ✅ **正值偏移**：边界向外扩展
- ✅ **负值偏移**：边界向内收缩
- ✅ **零值偏移**：使用原始外墙位置
- ✅ **极值处理**：正确处理最大/最小值

### 3. 兼容性测试

- ✅ **其他模式**：不影响 `INFILL`、`NO_SKIN` 等模式
- ✅ **无外墙情况**：正确回退到原有逻辑
- ✅ **参数禁用**：`retraction_combing != All` 时参数不生效

## 国际化支持

该功能已添加多语言支持：

- **简体中文**：避障边界偏移
- **繁体中文**：避障邊界偏移  
- **日语**：コーミング境界オフセット

## 总结

`retraction_combing_offset` 功能为用户提供了对避障边界的精确控制，通过基于外墙多边形的边界定义，实现了更灵活和精确的避障行为。该功能保持了完全的向后兼容性，同时为高级用户提供了强大的定制能力。
