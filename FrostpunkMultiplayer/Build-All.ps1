<#
.SYNOPSIS
Builds the complete Frostpunk Multiplayer mod package
#>

param(
    [string]$Configuration = "Release",
    [string]$Version = "1.0.0",
    [switch]$Clean,
    [switch]$Package
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Frostpunk Multiplayer Build System" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Configuration: $Configuration" -ForegroundColor Gray
Write-Host "Version: $Version" -ForegroundColor Gray
Write-Host ""

# Build Mod DLL
Write-Host "Building Mod DLL..." -ForegroundColor Yellow
& "$scriptDir\Build-ModDLL.ps1" -Configuration $Configuration -Clean:$Clean
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Build Launcher
Write-Host "Building Launcher..." -ForegroundColor Yellow
& "$scriptDir\Build-Launcher.ps1" -Configuration $Configuration -Clean:$Clean
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Package) {
    Write-Host "Packaging Release..." -ForegroundColor Yellow
    & "$scriptDir\Package-Release.ps1" -Version $Version
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host "  Build Complete!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host "Output: $scriptDir\Release\" -ForegroundColor Gray
if ($Package) {
    Write-Host "Package: $scriptDir\Release_Package\" -ForegroundColor Gray
}