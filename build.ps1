param([string]$OutputDirectory, [string]$IntermediateDirectory)
$ErrorActionPreference = 'Stop'
$toolRoot = $PSScriptRoot
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $toolRoot 'dist'
}
$header = Join-Path $toolRoot 'vendor\openvr.h'
if ((Get-FileHash -LiteralPath $header).Hash -ne '1E6ED57199896CC1F7C5484E50FA18955E97BE15BE690BEB28D998C877EAD7FD') {
    throw 'Unexpected OpenVR header. Use the pinned Valve SDK source in vendor.'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'MSVC x64 build tools are required.' }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $IntermediateDirectory) {
    $IntermediateDirectory = Join-Path $toolRoot '.build'
}
$IntermediateDirectory = [IO.Path]::GetFullPath($IntermediateDirectory)
New-Item -ItemType Directory -Force -Path $IntermediateDirectory | Out-Null
Push-Location $IntermediateDirectory
try {
    & cmd /d /s /c "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /MT /O2 /permissive- /utf-8 `"$toolRoot\core_tests.cpp`" /Fe:core_tests.exe"
    if ($LASTEXITCODE) { throw 'Test compilation failed.' }
    & .\core_tests.exe
    if ($LASTEXITCODE) { throw 'Unit tests failed.' }
    & cmd /d /s /c "`"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /MT /O2 /permissive- /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX `"$toolRoot\main.cpp`" /Fe:`"$OutputDirectory\SteamVREyeCalibrator.exe`" /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib bcrypt.lib d3d11.lib dxgi.lib dwmapi.lib windowsapp.lib windowscodecs.lib"
    if ($LASTEXITCODE) { throw 'Application compilation failed.' }
    Copy-Item -LiteralPath (Join-Path $toolRoot 'README.md') -Destination $OutputDirectory
    Copy-Item -LiteralPath (Join-Path $toolRoot 'vendor\LICENSE-OpenVR.txt') -Destination $OutputDirectory
    Get-FileHash -LiteralPath (Join-Path $OutputDirectory 'SteamVREyeCalibrator.exe')
} finally { Pop-Location }
