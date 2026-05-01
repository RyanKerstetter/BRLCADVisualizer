# Copyright (c) 2026 BRL-CAD Visualizer contributors
# SPDX-License-Identifier: MIT

param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "build-clean"),
    [string]$OutputDir = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

function Get-CacheValue {
    param(
        [string[]]$Lines,
        [string]$Name
    )

    $prefix = "${Name}:"
    foreach ($line in $Lines) {
        if ($line.StartsWith($prefix)) {
            $parts = $line.Split("=", 2)
            if ($parts.Length -eq 2) {
                return $parts[1].Trim()
            }
        }
    }

    throw "Missing CMake cache entry: $Name"
}

function Get-DumpbinPath {
    $candidate = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $candidate) {
        throw "dumpbin.exe not found."
    }
    return $candidate
}

function Get-WindeployQtPath {
    param([string]$Qt6Dir)

    $candidate = Join-Path $Qt6Dir "..\..\..\bin\windeployqt.exe"
    $resolved = [System.IO.Path]::GetFullPath($candidate)
    if (-not (Test-Path $resolved)) {
        throw "windeployqt.exe not found at $resolved"
    }
    return $resolved
}

function Get-VcRedistDir {
    $base = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC"
    if (-not (Test-Path $base)) {
        return $null
    }

    $candidate = Get-ChildItem $base -Recurse -Directory -Filter Microsoft.VC143.CRT -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\x64\\Microsoft\.VC143\.CRT$" -and $_.FullName -notmatch "\\onecore\\" } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if (-not $candidate) {
        return $null
    }
    return $candidate.FullName
}

function Get-PeDependents {
    param(
        [string]$Dumpbin,
        [string]$Path
    )

    $output = & $Dumpbin /nologo /dependents $Path 2>$null
    $deps = New-Object System.Collections.Generic.List[string]
    $capture = $false

    foreach ($line in $output) {
        if ($line -match "Image has the following dependencies:") {
            $capture = $true
            continue
        }

        if (-not $capture) {
            continue
        }

        $trimmed = $line.Trim()
        if (-not $trimmed) {
            continue
        }

        if ($trimmed -match "Summary") {
            break
        }

        if ($trimmed -match "^[A-Za-z0-9_.-]+\.dll$" -or $trimmed -match "^[A-Za-z0-9_.-]+\.exe$") {
            $deps.Add($trimmed)
        }
    }

    return $deps
}

function Copy-IfExists {
    param(
        [string]$Source,
        [string]$Destination
    )

    if (Test-Path $Source) {
        $destDir = Split-Path -Parent $Destination
        if ($destDir) {
            New-Item -ItemType Directory -Force -Path $destDir | Out-Null
        }
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
        return $true
    }

    return $false
}

$cachePath = Join-Path $BuildDir "CMakeCache.txt"
if (-not (Test-Path $cachePath)) {
    throw "Build directory not configured: $BuildDir"
}

$cacheLines = Get-Content $cachePath
$qt6Dir = Get-CacheValue $cacheLines "Qt6_DIR"
$osprayPrefix = Get-CacheValue $cacheLines "OSPRAY_PREFIX"
$rkcommonPrefix = Get-CacheValue $cacheLines "RKCOMMON_PREFIX"
$embreePrefix = Get-CacheValue $cacheLines "EMBREE_PREFIX"
$openvklPrefix = Get-CacheValue $cacheLines "OPENVKL_PREFIX"
$brlcadPrefix = Get-CacheValue $cacheLines "BRLCAD_PREFIX"
$tbbRoot = Get-CacheValue $cacheLines "TBB_ROOT"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$moduleRelease = Join-Path $repoRoot "bcs\Release\ospray_module_brl_cad.dll"
$releaseDir = Join-Path $BuildDir "Release"
$outputRoot = if ($OutputDir) { $OutputDir } else { Join-Path $BuildDir "IBRT" }
$outputRoot = [System.IO.Path]::GetFullPath($outputRoot)
$modelsDir = Join-Path $outputRoot "models"
$vcRedistDir = Get-VcRedistDir

if (-not $SkipBuild) {
    & cmake --build $repoRoot\bcs --config Release --target ospray_module_brl_cad
    if ($LASTEXITCODE -ne 0) {
        throw "Release BRL-CAD module build failed."
    }

    & cmake --build $BuildDir --config Release --target QtOsprayViewer
    if ($LASTEXITCODE -ne 0) {
        throw "Release viewer build failed."
    }
}

if (-not (Test-Path $moduleRelease)) {
    throw "Release ospray_module_brl_cad.dll not found at $moduleRelease"
}

if (Test-Path $outputRoot) {
    Remove-Item -LiteralPath $outputRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
New-Item -ItemType Directory -Force -Path $modelsDir | Out-Null

Copy-Item -LiteralPath (Join-Path $releaseDir "IBRT.exe") -Destination (Join-Path $outputRoot "IBRT.exe") -Force
Copy-Item -LiteralPath (Join-Path $releaseDir "IBRTRenderWorker.exe") -Destination (Join-Path $outputRoot "IBRTRenderWorker.exe") -Force
Copy-Item -LiteralPath $moduleRelease -Destination (Join-Path $outputRoot "ospray_module_brl_cad.dll") -Force

$dbDir = Join-Path $brlcadPrefix "share\db"
foreach ($demoName in @("moss.g", "havoc.g", "axis.g")) {
    $demoSource = Join-Path $dbDir $demoName
    if (Test-Path $demoSource) {
        Copy-Item -LiteralPath $demoSource -Destination (Join-Path $modelsDir $demoName) -Force
    }
}

$windeployqt = Get-WindeployQtPath $qt6Dir
& $windeployqt --release --no-translations --dir $outputRoot (Join-Path $outputRoot "IBRT.exe")
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed."
}

$sourceDirs = @(
    "C:\ospray\build\ospray\build\Release",
    "C:\ospray\build\rkcommon\build\Release",
    "C:\ospray\build\embree\src\bin",
    "C:\ospray\build\tbb\src\redist\intel64\vc14",
    $releaseDir,
    (Join-Path $osprayPrefix "bin"),
    (Join-Path $rkcommonPrefix "bin"),
    (Join-Path $embreePrefix "bin"),
    (Join-Path $openvklPrefix "bin"),
    (Join-Path $brlcadPrefix "bin"),
    (Join-Path $tbbRoot "redist\intel64\vc14"),
    $vcRedistDir
) | Where-Object { $_ -and (Test-Path $_) }

$dumpbin = Get-DumpbinPath
$systemNames = @(
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll", "ole32.dll",
    "oleaut32.dll", "ws2_32.dll", "ucrtbase.dll", "comdlg32.dll", "comctl32.dll",
    "imm32.dll", "winmm.dll", "version.dll", "setupapi.dll", "secur32.dll", "dbghelp.dll",
    "crypt32.dll", "shlwapi.dll", "rpcrt4.dll", "netapi32.dll", "mpr.dll", "dnsapi.dll",
    "dwmapi.dll", "uxtheme.dll", "opengl32.dll"
)

$queue = New-Object System.Collections.Generic.Queue[string]
$seenFiles = New-Object System.Collections.Generic.HashSet[string]([System.StringComparer]::OrdinalIgnoreCase)
$copiedNames = New-Object System.Collections.Generic.HashSet[string]([System.StringComparer]::OrdinalIgnoreCase)

Get-ChildItem -Path $outputRoot -Recurse -File -Include *.exe,*.dll | ForEach-Object {
    $queue.Enqueue($_.FullName)
    $null = $seenFiles.Add($_.FullName)
    $null = $copiedNames.Add($_.Name)
}

while ($queue.Count -gt 0) {
    $current = $queue.Dequeue()
    foreach ($dep in Get-PeDependents -Dumpbin $dumpbin -Path $current) {
        $depLower = $dep.ToLowerInvariant()
        if ($depLower.StartsWith("api-ms-win-") -or $depLower.StartsWith("ext-ms-")) {
            continue
        }
        if ($systemNames -contains $depLower) {
            continue
        }
        if ($depLower.EndsWith("_debug.dll") -or $depLower -match "qt6.+d\.dll$") {
            continue
        }
        if ($copiedNames.Contains($dep)) {
            continue
        }

        $found = $false
        foreach ($sourceDir in $sourceDirs) {
            $candidate = Join-Path $sourceDir $dep
            if (Test-Path $candidate) {
                $destination = Join-Path $outputRoot $dep
                Copy-Item -LiteralPath $candidate -Destination $destination -Force
                $null = $copiedNames.Add($dep)
                if ($seenFiles.Add($destination)) {
                    $queue.Enqueue($destination)
                }
                $found = $true
                break
            }
        }

        if (-not $found) {
            Write-Warning "Unresolved local dependency: $dep (referenced by $current)"
        }
    }
}

Get-ChildItem -Path $outputRoot -Recurse -File | Where-Object {
    $_.Name -match "_debug\.dll$" -or
    $_.Name -match "\.pdb$" -or
    $_.Name -match "\.log$" -or
    $_.Name -eq "tbb12_debug.dll" -or
    $_.Name -eq "tbbmalloc_debug.dll" -or
    $_.Name -eq "tbbmalloc_proxy_debug.dll"
} | Remove-Item -Force

if ($vcRedistDir) {
    foreach ($runtimeName in @(
        "concrt140.dll",
        "msvcp140.dll",
        "msvcp140_1.dll",
        "msvcp140_2.dll",
        "vccorlib140.dll",
        "vcruntime140.dll",
        "vcruntime140_1.dll",
        "vcruntime140_threads.dll"
    )) {
        $runtimeSource = Join-Path $vcRedistDir $runtimeName
        if (Test-Path $runtimeSource) {
            Copy-Item -LiteralPath $runtimeSource -Destination (Join-Path $outputRoot $runtimeName) -Force
        }
    }
}

Write-Host "Packaged Release runtime to $outputRoot"
