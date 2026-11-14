@echo off
echo Starting launcher...
"G:\claude_code\FrostpunkMultiplayer\Release\FrostpunkMultiplayerLauncher.exe" --new-instance > "G:\claude_code\FrostpunkMultiplayer\launcher_output.txt" 2>&1
echo Launcher exit code: %ERRORLEVEL% >> "G:\claude_code\FrostpunkMultiplayer\launcher_output.txt"
type "G:\claude_code\FrostpunkMultiplayer\launcher_output.txt"
