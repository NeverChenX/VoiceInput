@echo off
chcp 65001 >nul

reg delete "HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run" /v VoiceInput /f >nul 2>&1

if %ERRORLEVEL% equ 0 (
    echo 已取消 voice_input.exe 的开机启动。
) else (
    echo 未找到开机启动项，或删除失败。
)
pause
