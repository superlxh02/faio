<#
.SYNOPSIS
构建并验证 Windows 的 MSVC、Clang 和 MinGW 工具链矩阵。
.DESCRIPTION
原生 Windows 网络、文件和目录测试与可移植旧测试/有限示例均运行；
CTest 的超时/退出码直接令矩阵失败。构建日志和环境清单留在 build/windows-matrix。
#>
[CmdletBinding()]
param([ValidateSet('msvc','clang','mingw')][string[]]$Compilers = @('msvc','clang','mingw'),
      [switch]$Offline, [switch]$SkipInstalledHeaders, [int]$Jobs = 4)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$reportRoot = Join-Path $repo 'build\windows-matrix'
New-Item -ItemType Directory -Force -Path $reportRoot | Out-Null
& (Join-Path $PSScriptRoot 'bootstrap_windows.ps1') -Offline:$Offline
Push-Location $repo
try {
    foreach ($compiler in $Compilers) {
        . (Join-Path $PSScriptRoot 'windows_environment.ps1') -Compiler $compiler
        $preset = "windows-$compiler"
        & cmake --fresh --preset $preset 2>&1 | Tee-Object (Join-Path $reportRoot "$compiler-configure.log")
        if ($LASTEXITCODE -ne 0) { throw "$compiler configure failed" }
        & cmake --build --preset $preset --clean-first -j $Jobs 2>&1 | Tee-Object (Join-Path $reportRoot "$compiler-build.log") |
            Where-Object { $_ -notmatch '^(Note: including file:|注意: 包含文件:)' }
        if ($LASTEXITCODE -ne 0) { throw "$compiler build failed" }
        & ctest --preset $preset --output-on-failure 2>&1 | Tee-Object (Join-Path $reportRoot "$compiler-test.log")
        if ($LASTEXITCODE -ne 0) { throw "$compiler tests failed" }
        if ($compiler -eq 'mingw') {
            # 实际发现的 emutls 析构故障可能一次通过；100 次真实多 TU 生命周期回归。
            & ctest --preset $preset --repeat until-fail:100 -R '^faio_windows_multi_tu_tests$' --output-on-failure 2>&1 |
                Tee-Object (Join-Path $reportRoot 'mingw-multi-tu-stress.log')
            if ($LASTEXITCODE -ne 0) { throw 'MinGW multi-TU TLS lifecycle stress failed' }
        }
        if (-not $SkipInstalledHeaders) {
            & python (Join-Path $PSScriptRoot 'check_installed_headers.py') --build "build/$preset" 2>&1 | Tee-Object (Join-Path $reportRoot "$compiler-installed-headers.log")
            if ($LASTEXITCODE -ne 0) { throw "$compiler installed headers or consumer failed" }
        }
    }
} finally { Pop-Location }
Write-Host "Windows matrix completed: $reportRoot"
