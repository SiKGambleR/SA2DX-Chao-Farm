$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
	throw "Visual Studio locator not found: $vswhere"
}

$installPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installPath)) {
	throw 'Visual Studio 2022 x86 C++ Build Tools were not found.'
}

$vcvars = Join-Path $installPath 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
	throw "Visual Studio environment script not found: $vcvars"
}

$outputDirectory = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$objectDirectory = Join-Path $outputDirectory 'test-objects'
New-Item -ItemType Directory -Force -Path $objectDirectory | Out-Null
$output = Join-Path $outputDirectory 'GardenPoolStoreTests.exe'
$compilerDatabase = Join-Path $objectDirectory 'GardenPoolStoreTests.pdb'
$include = Join-Path $projectRoot 'src'
$overlaySource = Join-Path $projectRoot 'src\ChaoPoolOverlay.cpp'
$storeSource = Join-Path $projectRoot 'src\GardenPoolStore.cpp'
$checksumSource = Join-Path $projectRoot 'tests\ChaoSaveChecksum.cpp'
$testSource = Join-Path $projectRoot 'tests\GardenPoolStoreTests.cpp'
$command = 'call "' + $vcvars + '" x86 >nul && cd /d "' + $objectDirectory +
	'" && cl /nologo /std:c++17 /EHsc /W4 /WX /I"' +
	$include + '" /Fd:"' + $compilerDatabase +
	'" /Fe:"' + $output + '" "' + $overlaySource + '" "' + $storeSource + '" "' +
	$checksumSource + '" "' + $testSource +
	'" && "' + $output + '"'

& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) {
	throw "Garden sidecar tests failed with exit code $LASTEXITCODE."
}

Write-Output 'Garden sidecar tests passed.'
