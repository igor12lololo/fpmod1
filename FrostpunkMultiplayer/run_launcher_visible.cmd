@echo off
cd /d "G:\claude_code\FrostpunkMultiplayer\Release"
echo Starting launcher...
"G:\claude_code\FrostpunkMultiplayer\Release\FrostpunkMultiplayerLauncher.exe" --new-instance
echo Launcher exit code: %ERRORLEVEL%
timeout /t 5
