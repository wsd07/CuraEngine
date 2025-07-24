# CuraEngine 与 Cura 联调配置说明

## 🔗 联调概述

CuraEngine可以通过网络连接与Cura进行联调，这样您可以在CLion中调试CuraEngine，同时使用Cura的图形界面进行切片操作。

## 📋 联调步骤

### 1. 启动Cura
首先启动Cura应用程序，Cura会在端口49674上监听CuraEngine的连接。

### 2. 在CLion中配置运行参数
我已经为您创建了专门的联调运行配置：

#### 可用的运行配置：
- **CuraEngine Connect Debug** - 用于调试模式联调
- **CuraEngine Connect Release** - 用于发布模式联调

这些配置会自动运行命令：`curaengine connect 127.0.0.1:49674`

### 3. 在CLion中运行联调

#### 方法1：使用预配置的运行配置
1. 在CLion顶部的运行配置下拉菜单中选择：
   - `CuraEngine Connect Debug` (推荐用于调试)
   - `CuraEngine Connect Release`
2. 点击运行按钮 ▶️ 或按 `Shift+F10`
3. 或者点击调试按钮 🐛 或按 `Shift+F9` 进行调试

#### 方法2：手动添加运行参数
如果预配置的运行配置不可用，您可以手动设置：

1. 选择任意CuraEngine运行配置
2. 点击配置名称旁边的下拉箭头
3. 选择 "Edit Configurations..."
4. 在 "Program arguments" 字段中输入：`connect 127.0.0.1:49674`
5. 点击 "OK" 保存

### 4. 验证连接
成功连接后，您应该看到：
- CuraEngine在控制台输出连接信息
- Cura界面显示已连接到外部CuraEngine
- 在Cura中进行切片操作时，会调用您在CLion中运行的CuraEngine

## 🐛 调试技巧

### 设置断点
1. 在CuraEngine源代码中设置断点
2. 使用 `CuraEngine Connect Debug` 配置启动调试
3. 在Cura中执行切片操作
4. CLion会在断点处暂停，允许您检查变量和调用栈

### 常用调试位置
- `src/main.cpp` - 程序入口点
- `src/FffProcessor.cpp` - 主要的切片处理逻辑
- `src/FffGcodeWriter.cpp` - G-code生成逻辑
- `src/communication/ArcusCommunication.cpp` - 与Cura的通信逻辑

## 🔧 故障排除

### 连接失败
如果连接失败，请检查：
1. Cura是否正在运行
2. 端口49674是否被其他程序占用
3. 防火墙是否阻止了连接

### 端口被占用
如果端口49674被占用，您可以：
1. 关闭占用端口的程序
2. 或者在Cura中配置使用不同的端口

### 调试信息
在CuraEngine运行时，您可以看到详细的调试信息，包括：
- 网络连接状态
- 接收到的切片参数
- 处理进度
- 生成的G-code信息

## 📝 使用示例

### 基本联调流程：
1. 启动Cura
2. 在CLion中选择 `CuraEngine Connect Debug`
3. 点击调试按钮 🐛
4. 在关键函数处设置断点
5. 在Cura中加载模型并开始切片
6. 在CLion中观察代码执行和变量值

### 高级调试：
- 使用条件断点来捕获特定情况
- 使用监视窗口观察关键变量
- 使用调用栈分析函数调用路径
- 使用内存视图检查数据结构

## 🎯 开发建议

1. **先熟悉代码结构** - 了解CuraEngine的主要模块和数据流
2. **使用小模型测试** - 用简单的模型进行调试，减少复杂性
3. **关注关键路径** - 重点调试切片算法的核心部分
4. **记录修改** - 记录您的修改和测试结果

现在您可以开始CuraEngine与Cura的联调开发了！🚀
