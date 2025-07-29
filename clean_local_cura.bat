@echo off
setlocal enabledelayedexpansion

echo ========================================
echo 本地Cura文件夹清理脚本
echo 清理本地Cura文件夹，只保留CuraEngine.exe
echo 注意：此脚本只清理本地文件，不影响GitHub仓库
echo ========================================

set "SCRIPT_DIR=%~dp0"
set "CURA_DIR=%SCRIPT_DIR%..\Cura"

if not exist "%CURA_DIR%" (
    echo ERROR: Cura directory not found: %CURA_DIR%
    pause
    exit /b 1
)

echo 正在清理本地Cura文件夹: %CURA_DIR%
cd /d "%CURA_DIR%"

echo Step 1: 备份CuraEngine.exe...
if exist "CuraEngine.exe" (
    copy "CuraEngine.exe" "CuraEngine_backup.exe" >nul 2>&1
    echo INFO: CuraEngine.exe已备份
) else (
    echo WARNING: CuraEngine.exe not found in Cura directory
)

echo Step 2: 删除所有文件（除了CuraEngine.exe和.git）...
for /f "delims=" %%i in ('dir /b /a-d ^| findstr /v /i "CuraEngine"') do (
    echo 删除文件: %%i
    del "%%i" 2>nul
)

echo Step 3: 删除所有目录（除了.git）...
for /f "delims=" %%i in ('dir /b /ad ^| findstr /v /i "\.git"') do (
    echo 删除目录: %%i
    rmdir /s /q "%%i" 2>nul
)

echo Step 4: 恢复CuraEngine.exe...
if exist "CuraEngine_backup.exe" (
    move "CuraEngine_backup.exe" "CuraEngine.exe" >nul 2>&1
    echo INFO: CuraEngine.exe已恢复
)

echo ========================================
echo 本地清理完成！
echo 当前Cura文件夹内容：
dir /b
echo ========================================
echo 注意：这只是本地清理，GitHub仓库保持完整
echo ========================================

pause
