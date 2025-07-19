# CuraEngine 快速开始指南

## 🚀 一键构建和部署

```cmd
build_curaengine.bat
```

## 📋 可用选项

### 基本用法
```cmd
# 完整构建和部署（首次使用或依赖项变更后）
build_curaengine.bat

# 快速构建（推荐用于代码修改后）
build_curaengine.bat --quick

# 仅清理构建目录
build_curaengine.bat --clean

# 显示帮助信息
build_curaengine.bat --help
```

### 高级选项
```cmd
# 快速构建但不部署到Cura目录
build_curaengine.bat --quick --no-deploy

# 构建但不提交到Git
build_curaengine.bat --no-git

# 组合选项（快速构建，不部署，不提交）
build_curaengine.bat -q -nd -ng
```

## 🔧 脚本功能说明

### 完整构建模式 (`build_curaengine.bat`)
1. **清理** 之前的构建
2. **安装** 所有依赖项（通过Conan）
3. **配置** CMake（使用Visual Studio环境）
4. **构建** CuraEngine.exe（Release模式）
5. **测试** 可执行文件
6. **复制** 到Cura目录：`C:\Users\wsd07\vscode\Cura-Dev\Cura\CuraEngine.exe`
7. **提交** 并 **推送** 到GitHub：https://github.com/wsd07/Cura

### 快速构建模式 (`--quick`)
1. **清理** 之前的构建
2. **跳过** 依赖项安装（使用现有依赖）
3. **跳过** CMake配置（如果构建文件存在）
4. **构建** CuraEngine.exe
5. **测试** 可执行文件
6. **部署** 和 **提交**（除非指定跳过）

## 📋 系统要求

- ✅ Visual Studio 2022 Community
- ✅ Python 3.12+ 和 pip
- ✅ Git（已配置GitHub访问权限）
- ✅ Conan 2.7.1+ (`pip install conan`)
- ✅ Ninja构建系统 (`pip install ninja`)

## ✅ 成功指示器

查找这些消息：
- ✅ "成功: CuraEngine.exe工作正常"
- ✅ "成功: CuraEngine.exe已复制到 Cura目录"
- ✅ "成功: 更改已推送到GitHub仓库"
- ✅ "所有操作已成功完成！"

## 🐛 故障排除

### 构建失败
- 确保在CuraEngine根目录运行脚本
- 确认Visual Studio 2022已安装
- 检查网络连接（Conan下载需要）

### Git推送失败
- 检查GitHub凭据：`git config --global user.name`
- 验证网络连接
- 手动推送：`cd C:\Users\wsd07\vscode\Cura-Dev\Cura && git push`

### 快速构建失败
- 如果是首次构建，请先运行完整构建
- 确保依赖项已正确安装

## 📁 生成的文件

- `build\Release\CuraEngine.exe` - 构建的可执行文件
- `C:\Users\wsd07\vscode\Cura-Dev\Cura\CuraEngine.exe` - 部署的可执行文件
- GitHub提交（包含时间戳）

## 🎯 推荐工作流程

1. **首次设置**：`build_curaengine.bat`
2. **代码修改后**：`build_curaengine.bat --quick`
3. **依赖项变更后**：`build_curaengine.bat`
4. **仅测试构建**：`build_curaengine.bat --quick --no-deploy --no-git`

## 📊 性能对比

| 模式 | 时间 | 适用场景 |
|------|------|----------|
| 完整构建 | ~5-10分钟 | 首次构建、依赖项变更 |
| 快速构建 | ~1-2分钟 | 源码修改、日常开发 |
| 仅清理 | ~1秒 | 清理构建文件 |

---

**需要帮助？** 查看 `BUILD_INSTRUCTIONS.md` 获取详细文档。
