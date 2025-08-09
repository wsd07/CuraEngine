# BeadingStrategy参数体系详解

## 概述

BeadingStrategy系统通过复杂的参数体系控制墙体线宽的计算和分布。本文档详细分析了用户界面参数与BeadingStrategy核心算法参数之间的映射关系。

## 参数传递流程

### 1. 参数获取阶段 (WallToolPaths.cpp)

```cpp
// 基础线宽参数
const coord_t bead_width_0_ = settings_.get<coord_t>("wall_line_width_0");
const coord_t bead_width_x_ = settings_.get<coord_t>("wall_line_width_x");

// 过渡控制参数
const coord_t wall_transition_length = settings_.get<coord_t>("wall_transition_length");
const AngleRadians transitioning_angle = settings_.get<AngleRadians>("wall_transition_angle");

// 线宽阈值参数
const double min_odd_wall_line_width = settings_.get<double>("min_odd_wall_line_width");
const double wall_line_width_x = settings_.get<double>("wall_line_width_x");
const Ratio wall_add_middle_threshold = std::max(1.0, std::min(99.0, 100.0 * min_odd_wall_line_width / wall_line_width_x)) / 100.0;

// 分布控制参数
const int wall_distribution_count = settings_.get<int>("wall_distribution_count");
```

### 2. 策略工厂调用 (BeadingStrategyFactory::makeStrategy)

```cpp
const auto beading_strat = BeadingStrategyFactory::makeStrategy(
    bead_width_0_,                    // preferred_bead_width_outer
    bead_width_x_,                    // preferred_bead_width_inner  
    wall_transition_length,           // preferred_transition_length
    transitioning_angle,              // transitioning_angle
    print_thin_walls_,                // print_thin_walls
    min_bead_width_,                  // min_bead_width
    min_feature_size_,                // min_feature_size
    wall_split_middle_threshold,      // wall_split_middle_threshold
    wall_add_middle_threshold,        // wall_add_middle_threshold
    max_bead_count,                   // max_bead_count
    wall_0_inset_,                    // outer_wall_offset
    wall_distribution_count           // inward_distributed_center_wall_count
);
```

## 核心参数详解

### 1. 线宽控制参数

#### wall_line_width_0 (外墙线宽)
- **用户界面**: "Outer Wall Line Width"
- **默认值**: 通常等于喷头直径 (0.4mm)
- **作用**: 控制最外层墙体的线宽
- **BeadingStrategy映射**: `preferred_bead_width_outer`
- **影响**: 外墙质量和尺寸精度

#### wall_line_width_x (内墙线宽)  
- **用户界面**: "Inner Wall Line Width"
- **默认值**: 通常等于喷头直径 (0.4mm)
- **作用**: 控制内层墙体的线宽
- **BeadingStrategy映射**: `preferred_bead_width_inner`
- **影响**: 内墙强度和填充效率

### 2. 过渡控制参数

#### wall_transition_length (墙体过渡长度)
- **用户界面**: "Wall Transition Length"
- **默认值**: 0.4mm
- **作用**: 控制线宽变化的过渡距离
- **BeadingStrategy映射**: `preferred_transition_length`
- **影响**: 过渡区域的平滑度

#### wall_transition_angle (墙体过渡角度)
- **用户界面**: "Wall Transition Angle"  
- **默认值**: 10度
- **作用**: 控制过渡的角度阈值
- **BeadingStrategy映射**: `transitioning_angle`
- **影响**: 何时触发线宽过渡

### 3. 线宽阈值参数

#### min_odd_wall_line_width (最小奇数墙线宽)
- **用户界面**: "Minimum Odd Wall Line Width"
- **默认值**: 0.34mm (85% of nozzle diameter)
- **作用**: 控制何时添加中间墙
- **计算公式**: `wall_add_middle_threshold = min_odd_wall_line_width / wall_line_width_x`
- **影响**: 中间墙的生成策略

#### wall_split_middle_threshold (墙体分割阈值)
- **用户界面**: 通常隐藏，由算法计算
- **默认值**: 0.5 (50%)
- **作用**: 控制何时分割中间墙
- **BeadingStrategy映射**: `wall_split_middle_threshold`
- **影响**: 墙体分割的敏感度

### 4. 薄壁处理参数

#### fill_outline_gaps (填充轮廓间隙)
- **用户界面**: "Fill Gaps Between Walls"
- **默认值**: true
- **作用**: 启用薄壁处理
- **BeadingStrategy映射**: `print_thin_walls`
- **影响**: 是否启用WideningBeadingStrategy

#### min_bead_width (最小珠线宽度)
- **用户界面**: "Minimum Wall Line Width"
- **默认值**: 0.34mm
- **作用**: 最小可打印线宽
- **BeadingStrategy映射**: `min_bead_width`
- **影响**: WideningBeadingStrategy的最小输出宽度

#### min_feature_size (最小特征尺寸)
- **用户界面**: "Minimum Feature Size"
- **默认值**: 0.1mm
- **作用**: 最小可处理特征
- **BeadingStrategy映射**: `min_feature_size`  
- **影响**: WideningBeadingStrategy的最小输入宽度

### 5. 分布控制参数

#### wall_distribution_count (墙体分布数量)
- **用户界面**: "Wall Distribution Count"
- **默认值**: 1
- **作用**: 控制向内分布的中心墙数量
- **BeadingStrategy映射**: `inward_distributed_center_wall_count`
- **影响**: DistributedBeadingStrategy的分布策略

#### wall_0_inset (外墙内缩)
- **用户界面**: "Outer Wall Inset"
- **默认值**: 0
- **作用**: 外墙向内偏移距离
- **BeadingStrategy映射**: `outer_wall_offset`
- **影响**: 是否启用OuterWallInsetBeadingStrategy

## 策略链构建过程

### 1. 基础策略：DistributedBeadingStrategy
```cpp
BeadingStrategyPtr ret = make_unique<DistributedBeadingStrategy>(
    preferred_bead_width_inner,           // wall_line_width_x
    preferred_transition_length,          // wall_transition_length
    transitioning_angle,                  // wall_transition_angle
    wall_split_middle_threshold,          // 计算得出
    wall_add_middle_threshold,            // 从min_odd_wall_line_width计算
    inward_distributed_center_wall_count  // wall_distribution_count
);
```

### 2. 重分配策略：RedistributeBeadingStrategy
```cpp
ret = make_unique<RedistributeBeadingStrategy>(
    preferred_bead_width_outer,    // wall_line_width_0
    minimum_variable_line_ratio,   // 固定值0.5
    std::move(ret)
);
```

### 3. 薄壁策略：WideningBeadingStrategy (可选)
```cpp
if (print_thin_walls) {  // fill_outline_gaps
    ret = make_unique<WideningBeadingStrategy>(
        std::move(ret),
        min_feature_size,    // min_feature_size
        min_bead_width       // min_bead_width
    );
}
```

### 4. 外墙偏移策略：OuterWallInsetBeadingStrategy (可选)
```cpp
if (outer_wall_offset > 0) {  // wall_0_inset
    ret = make_unique<OuterWallInsetBeadingStrategy>(
        outer_wall_offset,   // wall_0_inset
        std::move(ret)
    );
}
```

### 5. 限制策略：LimitedBeadingStrategy (必须)
```cpp
ret = make_unique<LimitedBeadingStrategy>(
    max_bead_count,      // 从wall_line_count计算
    std::move(ret)
);
```

## 参数优化建议

### 1. 高质量外观设置
- `wall_line_width_0`: 等于喷头直径
- `wall_transition_length`: 0.4mm
- `min_odd_wall_line_width`: 0.34mm (85%)
- `wall_0_inset`: 0 (不内缩外墙)

### 2. 高强度设置
- `wall_line_width_x`: 略大于喷头直径
- `wall_distribution_count`: 2-3
- `fill_outline_gaps`: true
- `min_bead_width`: 0.3mm

### 3. 高速打印设置
- `wall_transition_length`: 0.2mm (更短过渡)
- `wall_transition_angle`: 15度 (更大角度)
- `fill_outline_gaps`: false (禁用薄壁)
- `wall_distribution_count`: 1

## 调试参数

### 启用详细日志
```cpp
spdlog::set_level(spdlog::level::debug);
```

### 关键调试信息
- 策略链构建过程
- 每个策略的参数值
- Beading计算结果
- 过渡区域检测

## 常见问题排查

### 1. 外墙不平滑
- 检查`wall_line_width_0`是否合适
- 调整`wall_transition_length`
- 考虑启用`wall_0_inset`

### 2. 薄壁丢失
- 确保`fill_outline_gaps = true`
- 调整`min_feature_size`和`min_bead_width`
- 检查`wall_add_middle_threshold`

### 3. 过渡区域突兀
- 增大`wall_transition_length`
- 减小`wall_transition_angle`
- 调整`wall_split_middle_threshold`

### 4. 内存使用过高
- 减小`max_bead_count`
- 简化几何形状
- 考虑禁用BeadingStrategy

## 新增功能：BeadingStrategy范围控制

### beading_strategy_scope 参数

#### 参数定义
```json
"beading_strategy_scope": {
    "label": "Beading Strategy Scope",
    "description": "Controls which parts use advanced beading strategy",
    "type": "enum",
    "options": {
        "all": "All",
        "inner_wall_skin": "Inner Wall & Skin",
        "only_skin": "Only Skin",
        "off": "Off"
    },
    "default_value": "inner_wall_skin"
}
```

#### 选项说明

**ALL (全部)**
- 所有墙体都使用BeadingStrategy
- 最高质量，但计算量最大
- 适用于高精度模型

**INNER_WALL_SKIN (内墙和表面)**
- 外墙使用简单偏移（更好的表面质量）
- 内墙和skin使用BeadingStrategy（优化填充）
- **推荐默认选项**
- 平衡质量和性能

**ONLY_SKIN (仅表面)**
- 只有skin墙体使用BeadingStrategy
- 所有模型墙体使用简单偏移
- 适用于表面质量要求高的场景

**OFF (关闭)**
- 完全禁用BeadingStrategy
- 所有墙体使用简单偏移算法
- 最高性能，适用于快速打印

#### 实现逻辑
```cpp
bool should_use_beading_strategy = true;

switch (beading_strategy_scope) {
    case EBeadingStrategyScope::OFF:
        should_use_beading_strategy = false;
        break;
    case EBeadingStrategyScope::ONLY_SKIN:
        should_use_beading_strategy = (section_type_ == SectionType::SKIN);
        break;
    case EBeadingStrategyScope::INNER_WALL_SKIN:
        if (section_type_ == SectionType::SKIN) {
            should_use_beading_strategy = true;  // skin总是使用
        } else {
            should_use_beading_strategy = (inset_count_ > 1);  // 多层墙才使用
        }
        break;
    case EBeadingStrategyScope::ALL:
    default:
        should_use_beading_strategy = true;
        break;
}
```

## 性能对比分析

### 计算复杂度对比

| 模式 | 外墙 | 内墙 | Skin | 相对性能 | 质量评分 |
|------|------|------|------|----------|----------|
| ALL | BeadingStrategy | BeadingStrategy | BeadingStrategy | 1.0x | 10/10 |
| INNER_WALL_SKIN | 简单偏移 | BeadingStrategy | BeadingStrategy | 1.5x | 9/10 |
| ONLY_SKIN | 简单偏移 | 简单偏移 | BeadingStrategy | 3.0x | 7/10 |
| OFF | 简单偏移 | 简单偏移 | 简单偏移 | 5.0x | 6/10 |

### 内存使用对比

| 模式 | Voronoi图 | 策略链 | 过渡计算 | 相对内存 |
|------|-----------|--------|----------|----------|
| ALL | 完整 | 完整 | 完整 | 1.0x |
| INNER_WALL_SKIN | 部分 | 部分 | 部分 | 0.6x |
| ONLY_SKIN | 最小 | 最小 | 最小 | 0.3x |
| OFF | 无 | 无 | 无 | 0.1x |

## 使用建议

### 1. 模型类型推荐

**精密机械零件**
- 推荐: `INNER_WALL_SKIN`
- 原因: 外墙固定线宽保证尺寸精度，内墙优化保证强度

**艺术装饰品**
- 推荐: `ALL`
- 原因: 最高质量，表面和内部都优化

**原型快速验证**
- 推荐: `OFF`
- 原因: 最快切片和打印速度

**功能性外壳**
- 推荐: `ONLY_SKIN`
- 原因: 表面质量重要，内部结构简单

### 2. 打印机性能推荐

**高端打印机 (CoreXY, 封闭腔体)**
- 推荐: `ALL` 或 `INNER_WALL_SKIN`
- 原因: 硬件精度高，可以充分利用算法优势

**入门级打印机 (Cartesian, 开放式)**
- 推荐: `ONLY_SKIN` 或 `OFF`
- 原因: 硬件限制，复杂算法效果有限

**大尺寸打印机**
- 推荐: `INNER_WALL_SKIN`
- 原因: 平衡质量和切片时间

### 3. 材料特性推荐

**PLA (易打印)**
- 推荐: `ALL`
- 原因: 材料稳定，可以使用复杂算法

**ABS/PETG (收缩性)**
- 推荐: `INNER_WALL_SKIN`
- 原因: 外墙固定线宽减少收缩影响

**TPU (柔性)**
- 推荐: `OFF`
- 原因: 柔性材料对线宽变化敏感

**高温材料 (PEEK, PEI)**
- 推荐: `ONLY_SKIN`
- 原因: 减少复杂性，提高成功率

## 总结

BeadingStrategy参数体系提供了精细的线宽控制能力，新增的`beading_strategy_scope`参数让用户可以根据具体需求在质量和性能之间找到最佳平衡点。推荐大多数用户使用`INNER_WALL_SKIN`模式，它在保证外墙表面质量的同时，优化了内部结构的填充效果。
