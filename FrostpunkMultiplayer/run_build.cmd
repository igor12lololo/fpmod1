@echo off
setlocal
set MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe
echo Using MSBuild: %MSBUILD%
"%MSBUILD%" "G:\claude_code\FrostpunkMultiplayer\build\FrostMenuMod.vcxproj" /p:Configuration=Release /p:Platform=x64 /p:OutDir="G:\claude_code\FrostpunkMultiplayer\Release\" /p:IntDir="G:\claude_code\FrostpunkMultiplayer\build\Release\" /maxcpucount /verbosity:detailed
echo Exit code: %ERRORLEVEL%
