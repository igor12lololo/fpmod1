<#
.SYNOPSIS
Builds the FrostMenuMod.dll for Frostpunk Multiplayer
#>

param(
    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$srcDir = Join-Path $scriptDir "src"
$buildDir = Join-Path $scriptDir "build"
$outDir = Join-Path $scriptDir "Release"

if ($Clean) {
    Write-Host "Cleaning build directories..." -ForegroundColor Yellow
    if (Test-Path $buildDir) { Remove-Item $buildDir -Recurse -Force }
    if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
}

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Find MSBuild
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = if (Test-Path $vswhere) {
    & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe
} else {
    "msbuild"
}

if (-not (Test-Path $msbuild) -and -not (Get-Command $msbuild -ErrorAction SilentlyContinue)) {
    throw "MSBuild not found. Install Visual Studio Build Tools."
}

# Create project file using single-quoted here-string to preserve $() and %() references
$projectFile = Join-Path $buildDir "FrostMenuMod.vcxproj"
$projectXml = @'
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64">
      <Configuration>Debug</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64">
      <Configuration>Release</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>17.0</VCProjectVersion>
    <ProjectGuid>{1A2B3C4D-5E6F-7A8B-9C0D-1E2F3A4B5C6D}</ProjectGuid>
    <Keyword>Win32Proj</Keyword>
    <RootNamespace>FrostMP</RootNamespace>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'" Label="Configuration">
    <ConfigurationType>DynamicLibrary</ConfigurationType>
    <UseDebugLibraries>true</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'" Label="Configuration">
    <ConfigurationType>DynamicLibrary</ConfigurationType>
    <UseDebugLibraries>false</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <CharacterSet>Unicode</CharacterSet>
    <WholeProgramOptimization>true</WholeProgramOptimization>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <ImportGroup Label="ExtensionSettings" />
  <ImportGroup Label="Shared" />
  <ImportGroup Label="PropertySheets" Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <Import Project="$(UserRootDir)\Microsoft.Cpp.$(Platform).user.props" Condition="exists('$(UserRootDir)\Microsoft.Cpp.$(Platform).user.props')" Label="LocalAppDataPlatform" />
  </ImportGroup>
  <ImportGroup Label="PropertySheets" Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <Import Project="$(UserRootDir)\Microsoft.Cpp.$(Platform).user.props" Condition="exists('$(UserRootDir)\Microsoft.Cpp.$(Platform).user.props')" Label="LocalAppDataPlatform" />
  </ImportGroup>
  <PropertyGroup Label="UserMacros" />
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <LinkIncremental>true</LinkIncremental>
    <OutDir>$(SolutionDir)$(Configuration)\</OutDir>
    <IntDir>$(Configuration)\</IntDir>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <LinkIncremental>false</LinkIncremental>
    <OutDir>$(SolutionDir)$(Configuration)\</OutDir>
    <IntDir>$(Configuration)\</IntDir>
  </PropertyGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <ClCompile>
      <WarningLevel>Level3</WarningLevel>
      <Optimization>Disabled</Optimization>
      <PreprocessorDefinitions>WIN32;_DEBUG;_WINDOWS;_USRDLL;FROSTMP_EXPORTS;_CRT_SECURE_NO_WARNINGS;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <ConformanceMode>true</ConformanceMode>
      <AdditionalIncludeDirectories>$(ProjectDir)..\src;$(ProjectDir)..\src\Common;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <LanguageStandard>stdcpp20</LanguageStandard>
    </ClCompile>
    <Link>
      <SubSystem>Windows</SubSystem>
      <GenerateDebugInformation>true</GenerateDebugInformation>
      <ModuleDefinitionFile>$(ProjectDir)..\src\ModDLL\FrostMenuMod.def</ModuleDefinitionFile>
    </Link>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <ClCompile>
      <WarningLevel>Level3</WarningLevel>
      <Optimization>MaxSpeed</Optimization>
      <FunctionLevelLinking>true</FunctionLevelLinking>
      <IntrinsicFunctions>true</IntrinsicFunctions>
      <PreprocessorDefinitions>WIN32;NDEBUG;_WINDOWS;_USRDLL;FROSTMP_EXPORTS;_CRT_SECURE_NO_WARNINGS;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <ConformanceMode>true</ConformanceMode>
      <AdditionalIncludeDirectories>$(ProjectDir)..\src;$(ProjectDir)..\src\Common;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <LanguageStandard>stdcpp20</LanguageStandard>
    </ClCompile>
    <Link>
      <SubSystem>Windows</SubSystem>
      <GenerateDebugInformation>true</GenerateDebugInformation>
      <EnableCOMDATFolding>true</EnableCOMDATFolding>
      <OptimizeReferences>true</OptimizeReferences>
      <ModuleDefinitionFile>$(ProjectDir)..\src\ModDLL\FrostMenuMod.def</ModuleDefinitionFile>
    </Link>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="..\src\Common\Common.h" />
    <ClCompile Include="..\src\Common\Logger.h" />
    <ClCompile Include="..\src\Network\NetworkProtocol.h" />
    <ClCompile Include="..\src\Network\SteamNetworking.h" />
    <ClCompile Include="..\src\Network\SteamNetworking.cpp" />
    <ClCompile Include="..\src\Network\NetworkManager.h" />
    <ClCompile Include="..\src\Network\NetworkManager.cpp" />
    <ClCompile Include="..\src\GameHooks\GameStructs.h" />
    <ClCompile Include="..\src\GameHooks\Memory.h" />
    <ClCompile Include="..\src\GameHooks\Memory.cpp" />
    <ClCompile Include="..\src\GameHooks\GameReader.h" />
    <ClCompile Include="..\src\GameHooks\GameReader.cpp" />
    <ClCompile Include="..\src\GameHooks\GameWriter.h" />
    <ClCompile Include="..\src\GameHooks\GameWriter.cpp" />
    <ClCompile Include="..\src\GameHooks\HookManager.h" />
    <ClCompile Include="..\src\GameHooks\HookManager.cpp" />
    <ClCompile Include="..\src\Overlay\OverlayUI.h" />
    <ClCompile Include="..\src\Overlay\OverlayUI.cpp" />
    <ClCompile Include="..\src\ModDLL\ModMain.h" />
    <ClCompile Include="..\src\ModDLL\ModMain.cpp" />
    <ClCompile Include="..\src\ModDLL\DllMain.cpp" />
  </ItemGroup>
  <ItemGroup>
    <ClInclude Include="..\src\Common\Common.h" />
    <ClInclude Include="..\src\Common\Logger.h" />
    <ClInclude Include="..\src\Network\NetworkProtocol.h" />
    <ClInclude Include="..\src\Network\SteamNetworking.h" />
    <ClInclude Include="..\src\Network\NetworkManager.h" />
    <ClInclude Include="..\src\GameHooks\GameStructs.h" />
    <ClInclude Include="..\src\GameHooks\Memory.h" />
    <ClInclude Include="..\src\GameHooks\GameReader.h" />
    <ClInclude Include="..\src\GameHooks\GameWriter.h" />
    <ClInclude Include="..\src\GameHooks\HookManager.h" />
    <ClInclude Include="..\src\Overlay\OverlayUI.h" />
    <ClInclude Include="..\src\ModDLL\ModMain.h" />
  </ItemGroup>
  <ItemGroup>
    <None Include="..\src\ModDLL\FrostMenuMod.def" />
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
  <ImportGroup Label="ExtensionTargets" />
</Project>
'@

$projectXml | Set-Content -Path $projectFile -Encoding UTF8

# Create module definition file
$defFile = Join-Path $srcDir "ModDLL\FrostMenuMod.def"
$defContent = @"
LIBRARY "FrostMenuMod"
EXPORTS
    FrostMP_InitializeHost @1
    FrostMP_InitializeClient @2
    FrostMP_Shutdown @3
    FrostMP_IsInitialized @4
    FrostMP_GetConnectionState @5
"@
$defContent | Set-Content -Path $defFile -Encoding ASCII

# Build
Write-Host "Building FrostMenuMod.dll ($Configuration)..." -ForegroundColor Cyan
$buildArgs = @(
    $projectFile,
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/p:OutDir=$outDir\",
    "/p:IntDir=$buildDir\$Configuration\",
    "/maxcpucount",
    "/verbosity:minimal"
)

$process = Start-Process -FilePath $msbuild -ArgumentList $buildArgs -Wait -PassThru -NoNewWindow
$exitCode = $process.ExitCode
if ($exitCode -ne 0) {
    throw "Build failed with exit code $exitCode"
}

$builtDll = Join-Path $outDir "FrostMenuMod.dll"
if (Test-Path $builtDll) {
    Write-Host "Build successful! Output: $builtDll" -ForegroundColor Green
} else {
    throw "DLL not found at expected location: $builtDll"
}