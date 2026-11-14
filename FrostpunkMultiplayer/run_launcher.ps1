$ErrorActionPreference = "Continue"
$launcher = Join-Path -Path "G:\claude_code\FrostpunkMultiplayer\Release" -ChildPath "FrostpunkMultiplayerLauncher.exe"
$logFile = Join-Path -Path "G:\claude_code\FrostpunkMultiplayer" -ChildPath "launcher_output.txt"
& $launcher --new-instance 2>&1 | Tee-Object -FilePath $logFile
Write-Host "Exit code: $LASTEXITCODE"
