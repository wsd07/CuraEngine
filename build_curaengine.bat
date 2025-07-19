@echo off
chcp 65001 >nul 2>&1
echo ========================================
echo CuraEngine Build and Deploy Script
echo ========================================

REM Set error handling
setlocal enabledelayedexpansion

REM Parse command line arguments
set "QUICK_BUILD=false"
set "CLEAN_ONLY=false"
set "NO_DEPLOY=false"
set "NO_GIT=false"
set "HELP=false"
set "PUSH_SOURCE=false"
set "PULL_SOURCE=false"

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="--quick" set "QUICK_BUILD=true"
if /i "%~1"=="-q" set "QUICK_BUILD=true"
if /i "%~1"=="--clean" set "CLEAN_ONLY=true"
if /i "%~1"=="-c" set "CLEAN_ONLY=true"
if /i "%~1"=="--no-deploy" set "NO_DEPLOY=true"
if /i "%~1"=="-nd" set "NO_DEPLOY=true"
if /i "%~1"=="--no-git" set "NO_GIT=true"
if /i "%~1"=="-ng" set "NO_GIT=true"
if /i "%~1"=="--help" set "HELP=true"
if /i "%~1"=="-h" set "HELP=true"
if /i "%~1"=="--push" set "PUSH_SOURCE=true"
if /i "%~1"=="--pull" set "PULL_SOURCE=true"
shift
goto parse_args

:args_done

REM Show help information
if "%HELP%"=="true" (
    echo.
    echo Usage: build_curaengine.bat [options]
    echo.
    echo Build Options:
    echo   --quick, -q      Quick build ^(skip dependencies, compile source only^)
    echo   --clean, -c      Clean build directory only, no build
    echo   --no-deploy, -nd Do not deploy to Cura directory
    echo   --no-git, -ng    Do not commit to Git repository
    echo   --help, -h       Show this help information
    echo.
    echo Source Code Management:
    echo   --push           Push CuraEngine source changes to GitHub
    echo   --pull           Pull latest CuraEngine source from GitHub
    echo.
    echo Examples:
    echo   build_curaengine.bat           # Full build and deploy
    echo   build_curaengine.bat --quick   # Quick build ^(recommended after code changes^)
    echo   build_curaengine.bat --clean   # Clean build directory only
    echo   build_curaengine.bat -q -nd    # Quick build but no deploy
    echo   build_curaengine.bat --pull    # Pull latest source code
    echo   build_curaengine.bat --push    # Push source code changes
    echo.
    pause
    exit /b 0
)

REM Check if we are in the correct directory
if not exist "conanfile.py" (
    echo ERROR: Please run this script from CuraEngine root directory
    pause
    exit /b 1
)

REM Handle source code management operations
if "%PULL_SOURCE%"=="true" (
    echo ========================================
    echo Pulling latest CuraEngine source code...
    echo ========================================

    echo Step 1: Checking Git repository status...
    git status --porcelain >nul 2>&1
    if !errorlevel! neq 0 (
        echo ERROR: This directory is not a Git repository
        echo Please ensure you are in a Git-managed CuraEngine directory
        pause
        exit /b 1
    )

    echo Step 2: Checking for uncommitted changes...
    for /f %%i in ('git status --porcelain') do (
        echo WARNING: You have uncommitted changes in your working directory
        echo Please commit or stash your changes before pulling
        git status
        pause
        exit /b 1
    )

    echo Step 3: Pulling latest changes from GitHub...
    git pull origin main
    if !errorlevel! neq 0 (
        echo ERROR: Failed to pull from GitHub repository
        echo Please check your network connection and repository access
        pause
        exit /b 1
    )

    echo SUCCESS: Latest source code pulled successfully
    echo You may want to run a full build: build_curaengine.bat
    pause
    exit /b 0
)

if "%PUSH_SOURCE%"=="true" (
    echo ========================================
    echo Pushing CuraEngine source changes...
    echo ========================================

    echo Step 1: Checking Git repository status...
    git status --porcelain >nul 2>&1
    if !errorlevel! neq 0 (
        echo ERROR: This directory is not a Git repository
        echo Please ensure you are in a Git-managed CuraEngine directory
        pause
        exit /b 1
    )

    echo Step 2: Checking for changes to commit...
    git diff --quiet --exit-code
    if !errorlevel! equ 0 (
        git diff --quiet --exit-code --cached
        if !errorlevel! equ 0 (
            echo INFO: No changes to commit
            echo Current status:
            git status
            pause
            exit /b 0
        )
    )

    echo Step 3: Showing current changes...
    git status
    echo.
    echo Step 4: Adding all changes...
    git add .

    echo Step 5: Committing changes...
    set /p commit_msg="Enter commit message (or press Enter for default): "
    if "!commit_msg!"=="" (
        set "commit_msg=Update CuraEngine source - %date% %time%"
    )
    git commit -m "!commit_msg!"

    echo Step 6: Pushing to GitHub...
    git push origin main
    if !errorlevel! neq 0 (
        echo ERROR: Failed to push to GitHub repository
        echo Please check your network connection and repository access
        pause
        exit /b 1
    )

    echo SUCCESS: Changes pushed to GitHub successfully
    pause
    exit /b 0
)

REM Clean only mode
if "%CLEAN_ONLY%"=="true" (
    echo Step: Cleaning build directory...
    if exist "build" (
        rmdir /s /q "build"
        echo SUCCESS: Build directory cleaned
    ) else (
        echo INFO: Build directory does not exist, nothing to clean
    )
    pause
    exit /b 0
)

echo Step 1: Cleaning previous build...
REM In quick build mode, don't clean if build directory exists
if "%QUICK_BUILD%"=="true" (
    if exist "build" (
        echo INFO: Keeping existing build directory for quick build
    ) else (
        echo INFO: No build directory to clean
    )
) else (
    if exist "build" rmdir /s /q "build"
)

REM Quick build mode skips dependency installation
if "%QUICK_BUILD%"=="true" (
    echo Step 2: Skipping dependency installation ^(quick build mode^)...
    echo INFO: Using existing Conan dependencies
    if not exist "build\Release\generators" (
        echo WARNING: Conan dependencies not found, need to run full build first
        echo HINT: Run build_curaengine.bat ^(without --quick parameter^)
        pause
        exit /b 1
    )
) else (
    echo Step 2: Installing Conan dependencies ^(correct Boost configuration^)...
    conan install . --build=missing -o "boost/*:header_only=False" -o "boost/*:without_exception=False"

    if !errorlevel! neq 0 (
        echo ERROR: Conan dependency installation failed
        pause
        exit /b 1
    )
)

echo Step 3: Setting up Visual Studio environment and configuring CMake...
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

REM Quick build mode may not need to reconfigure CMake
if "%QUICK_BUILD%"=="true" (
    if exist "build\Release\build.ninja" (
        echo INFO: Skipping CMake configuration ^(using existing configuration^)
        goto build_step
    ) else (
        echo INFO: Need to configure CMake ^(build files do not exist^)
    )
)

cmake --preset conan-release

if !errorlevel! neq 0 (
    echo ERROR: CMake configuration failed
    pause
    exit /b 1
)

:build_step
echo Step 4: Building CuraEngine...
cmake --build build\Release --config Release

if !errorlevel! neq 0 (
    echo ERROR: Build failed
    pause
    exit /b 1
)

echo Step 5: Verifying build results...
if not exist "build\Release\CuraEngine.exe" (
    echo ERROR: CuraEngine.exe was not generated
    pause
    exit /b 1
)

echo Step 6: Testing CuraEngine...
build\Release\CuraEngine.exe --help >nul 2>&1
if !errorlevel! neq 0 (
    echo WARNING: CuraEngine.exe may not be working properly
) else (
    echo SUCCESS: CuraEngine.exe is working correctly
)

REM Deploy step ^(optional^)
if "%NO_DEPLOY%"=="true" (
    echo INFO: Skipping deployment to Cura directory ^(--no-deploy option^)
    goto git_step
)

echo Step 7: Copying CuraEngine.exe to Cura directory...
set CURA_DIR=C:\Users\wsd07\vscode\Cura-Dev\Cura
if not exist "%CURA_DIR%" (
    echo ERROR: Cura directory not found: %CURA_DIR%
    pause
    exit /b 1
)

cmd /c copy "build\Release\CuraEngine.exe" "%CURA_DIR%\CuraEngine.exe" /Y
if !errorlevel! neq 0 (
    echo ERROR: Failed to copy CuraEngine.exe to Cura directory
    pause
    exit /b 1
)
echo SUCCESS: CuraEngine.exe copied to %CURA_DIR%

:git_step
REM Git commit step ^(optional^)
if "%NO_GIT%"=="true" (
    echo INFO: Skipping Git commit ^(--no-git option^)
    goto completion
)

if "%NO_DEPLOY%"=="true" (
    echo INFO: Skipping Git commit ^(no files deployed^)
    goto completion
)

echo Step 8: Pushing to GitHub repository...
cd /d "%CURA_DIR%"
git add CuraEngine.exe
git commit -m "Update CuraEngine.exe - Built on %date% %time%"
git push origin main

if !errorlevel! neq 0 (
    echo WARNING: Git push failed. Please check your GitHub credentials and network connection.
    echo You may need to push manually: cd %CURA_DIR% ^&^& git push origin main
) else (
    echo SUCCESS: Changes pushed to GitHub repository
)

:completion
echo ========================================
echo All operations completed successfully!
echo ========================================
echo Build location: %~dp0build\Release\CuraEngine.exe
if "%NO_DEPLOY%"=="false" (
    echo Deploy location: %CURA_DIR%\CuraEngine.exe
)
echo GitHub repository: https://github.com/wsd07/Cura
echo ========================================

REM Show build statistics
echo.
echo Build Statistics:
if "%QUICK_BUILD%"=="true" (
    echo - Build mode: Quick build ^(source code only^)
) else (
    echo - Build mode: Full build ^(including dependencies^)
)
if "%NO_DEPLOY%"=="true" (
    echo - Deploy status: Skipped
) else (
    echo - Deploy status: Completed
)
if "%NO_GIT%"=="true" (
    echo - Git commit: Skipped
) else (
    echo - Git commit: Completed
)
echo - Source repository: https://github.com/wsd07/CuraEngine

pause
