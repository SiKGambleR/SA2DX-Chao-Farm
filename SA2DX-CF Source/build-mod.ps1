$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$gameHeaders = 'G:\SteamLibrary\steamapps\common\Sonic Adventure 2\programming'
if (-not (Test-Path -LiteralPath $vswhere)) {
	throw "Visual Studio locator not found: $vswhere"
}
if (-not (Test-Path -LiteralPath (Join-Path $gameHeaders 'SA2ModLoader.h'))) {
	throw "SA2 Mod Loader headers were not found: $gameHeaders"
}

$installPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installPath)) {
	throw 'Visual Studio 2022 x86 C++ Build Tools were not found.'
}

$vcvars = Join-Path $installPath 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
	throw "Visual Studio environment script not found: $vcvars"
}

$outputDirectory = Join-Path $projectRoot 'build\SA2SixGardenExpansion'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$objectDirectory = Join-Path $outputDirectory 'objects'
New-Item -ItemType Directory -Force -Path $objectDirectory | Out-Null
$output = Join-Path $outputDirectory 'GardenExpansion.dll'
$compilerDatabase = Join-Path $objectDirectory 'GardenExpansion.pdb'
$pluginSource = Join-Path $projectRoot 'src\GardenExpansionPlugin.cpp'
$overlaySource = Join-Path $projectRoot 'src\ChaoPoolOverlay.cpp'
$storeSource = Join-Path $projectRoot 'src\GardenPoolStore.cpp'
$manifest = Join-Path $projectRoot 'mod\mod.ini'
Copy-Item -LiteralPath $manifest -Destination (Join-Path $outputDirectory 'mod.ini') -Force
Copy-Item -Path (Join-Path $projectRoot 'mod\*') -Destination $outputDirectory -Recurse -Force

$command = 'call "' + $vcvars + '" x86 >nul && cd /d "' + $objectDirectory +
	'" && cl /nologo /std:c++17 /EHsc /W4 /WX /LD /I"' +
	(Join-Path $projectRoot 'src') + '" /I"' + $gameHeaders + '" /Fd:"' +
	$compilerDatabase + '" /Fe:"' + $output +
	'" "' + $pluginSource + '" "' + $overlaySource + '" "' + $storeSource + '" "' + (Join-Path $projectRoot 'src\Sa2LvlLoader.cpp') + '" /link /MACHINE:X86 user32.lib'

& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) {
	throw "Garden expansion plugin build failed with exit code $LASTEXITCODE."
}

Write-Output "Built development mod package: $outputDirectory"
