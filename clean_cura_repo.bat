@echo off
setlocal enabledelayedexpansion

echo ========================================
echo Clean Cura Repository Script
echo Keep only CuraEngine.exe
echo ========================================

set "CURA_DIR=C:\Users\wsd07\vscode\Cura-Dev\Cura"

REM Check if Cura directory exists
if not exist "%CURA_DIR%" (
    echo ERROR: Cura directory not found: %CURA_DIR%
    pause
    exit /b 1
)

cd /d "%CURA_DIR%"

REM Check if this is a Git repository
if not exist ".git" (
    echo ERROR: %CURA_DIR% is not a Git repository
    pause
    exit /b 1
)

echo Step 1: Backing up CuraEngine.exe...
if exist "CuraEngine.exe" (
    copy "CuraEngine.exe" "CuraEngine.exe.backup" >nul
    echo SUCCESS: CuraEngine.exe backed up
) else (
    echo WARNING: CuraEngine.exe not found in Cura directory
)

echo Step 2: Removing all files except CuraEngine.exe and .git...
REM Remove all files except CuraEngine.exe and .git folder
for /f "delims=" %%i in ('dir /b /a-d 2^>nul ^| findstr /v /i "CuraEngine.exe"') do (
    echo Removing file: %%i
    del "%%i" 2>nul
)

REM Remove all directories except .git
for /f "delims=" %%i in ('dir /b /ad 2^>nul ^| findstr /v /i "\.git"') do (
    echo Removing directory: %%i
    rmdir /s /q "%%i" 2>nul
)

echo Step 3: Restoring CuraEngine.exe if needed...
if exist "CuraEngine.exe.backup" (
    if not exist "CuraEngine.exe" (
        move "CuraEngine.exe.backup" "CuraEngine.exe" >nul
        echo SUCCESS: CuraEngine.exe restored
    ) else (
        del "CuraEngine.exe.backup" 2>nul
    )
)

echo Step 4: Adding changes to Git...
git add .
git add -u

echo Step 5: Committing cleanup...
git commit -m "Clean repository - keep only CuraEngine.exe" 2>nul

echo Step 6: Force pushing to GitHub...
git push origin main --force

if !errorlevel! neq 0 (
    echo WARNING: Force push failed. Trying alternative method...
    git fetch origin main
    git reset --hard HEAD~1
    git add CuraEngine.exe
    git commit -m "Clean repository - keep only CuraEngine.exe"
    git push origin main --force
)

if !errorlevel! neq 0 (
    echo ERROR: Failed to push to GitHub
    echo Please check your network connection and GitHub credentials
    pause
    exit /b 1
) else (
    echo SUCCESS: Repository cleaned and pushed to GitHub
    echo Only CuraEngine.exe remains in the repository
)

echo.
echo Current repository contents:
dir /b

echo.
echo Repository cleanup completed successfully!
pause
