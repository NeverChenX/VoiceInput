@echo off
chcp 65001 >nul
set "EXE_PATH=E:\Share\Project\VoiceInput\Windows\voice_input.exe"

if not exist "%EXE_PATH%" (
    echo 错误：找不到 voice_input.exe
    pause
    exit /b 1
)

reg add "HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run" /v VoiceInput /t REG_SZ /d "%EXE_PATH%" /f >nul

if %ERRORLEVEL% equ 0 (
    echo 已成功将 voice_input.exe 设置为开机启动。
    echo 路径：%EXE_PATH%
) else (
    echo 设置失败，请检查权限。
)
pause
