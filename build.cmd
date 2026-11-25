@echo off
rem 编译 FlipRow，输出到 bin\FlipRow.exe。
rem 用法，build.cmd 编译，build.cmd run 编译并运行，build.cmd demo 编译并运行演示界面。
setlocal
set "ROOT=%~dp0"
cd /d "%~dp0."

where cl.exe >nul 2>nul && goto compilar
set "VCV="
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSW%" for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCV=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCV if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCV=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCV if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCV=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCV (
  echo [error] Visual Studio with C++ not found.
  exit /b 1
)
call "%VCV%" >nul || exit /b 1

:compilar
if not exist bin mkdir bin
if not exist obj mkdir obj
rem 编译图标和版本资源。
rc /nologo /i res /fo obj\app.res res\app.rc || exit /b 1
set LIBS=user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib d2d1.lib dwrite.lib d3d11.lib dxgi.lib dcomp.lib dwmapi.lib windowscodecs.lib version.lib advapi32.lib shcore.lib uuid.lib
cl /nologo /EHsc /O2 /W3 /std:c++20 /utf-8 /DUNICODE /D_UNICODE /Fo:obj\ /Fd:obj\ src\*.cpp obj\app.res /Fe:bin\FlipRow.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:res\app.manifest %LIBS% || exit /b 1
rem 清除文件可能继承的低完整性标签，带低标签的 exe 会被系统按低权限启动。
icacls "bin\FlipRow.exe" /setintegritylevel Medium >nul 2>nul
echo.
echo ok: bin\FlipRow.exe
if /i "%1"=="run" start "" "%ROOT%bin\FlipRow.exe"
if /i "%1"=="demo" start "" "%ROOT%bin\FlipRow.exe" --demo --debug
endlocal
exit /b 0
