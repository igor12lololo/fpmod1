$ErrorActionPreference = "Continue"
$launcher = "G:\claude_code\FrostpunkMultiplayer\Release\FrostpunkMultiplayerLauncher.exe"
$logFile = "G:\claude_code\FrostpunkMultiplayer\launcher_ps_output.txt"

Write-Host "Starting launcher: $launcher"
& $launcher --new-instance > $logFile 2>&1
Write-Host "Exit code: $LASTEXITCODE"

if (Test-Path $logFile) {
    Write-Host "=== Launcher output ==="
    Get-Content $logFile
}

Write-Host "=== All FrostMP logs in Release ==="
Get-ChildItem "G:\claude_code\FrostpunkMultiplayer\Release\FrostMP*.log" | Sort-Object Name | ForEach-Object {
    Write-Host "=== $($_.Name) ==="
    Get-Content $_.FullName
}
