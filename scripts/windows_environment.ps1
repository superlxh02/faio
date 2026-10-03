<#
.SYNOPSIS
以 dot-source 方式导入 MSVC/Clang/MinGW 编译环境，仅修改当前 PowerShell 进程。
#>
[CmdletBinding()]
param([ValidateSet('msvc','clang','mingw')][string]$Compiler = 'msvc',
      [string]$DependencyRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '..\faio-deps'))
$ErrorActionPreference = 'Stop'
$DependencyRoot = [IO.Path]::GetFullPath($DependencyRoot)
# MSVC 中文语言包的 /showIncludes 输出必须和 Ninja 的依赖前缀使用相同 UTF-8。
# 只有 VSLANG 不能切换未安装的英文资源；同时设置当前控制台和 PowerShell 编码。
$OutputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$env:PYTHONUTF8 = '1'
& cmd /d /c 'chcp 65001 >nul'
$env:VSLANG = '1033'
if ($Compiler -ne 'mingw') {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual Studio C++ x64 build tools not found' }
    $vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
    & cmd /d /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
    }
    if ($LASTEXITCODE -ne 0) { throw 'Failed to initialize Visual Studio compiler environment' }
    $env:VSLANG = '1033'
    if ($Compiler -eq 'clang') {
        $llvm = Join-Path $env:ProgramFiles 'LLVM\bin'
        if (Test-Path -LiteralPath "$llvm\clang-cl.exe") { $env:PATH = "$llvm;$env:PATH" }
        if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue)) { throw 'clang-cl.exe not found' }
        $clangResource = (& clang-cl -print-resource-dir).Trim()
        $env:PATH = "$(Join-Path $clangResource 'lib\windows');$env:PATH"
    }
} else {
    $mingwBin = Join-Path $DependencyRoot 'tools\msys2-ucrt64\ucrt64\bin'
    if (-not (Test-Path -LiteralPath "$mingwBin\g++.exe")) { throw 'Run bootstrap_windows.ps1 to unpack MinGW first' }
    $env:PATH = "$mingwBin;$env:PATH"
}
# 已安装的 rustup/rustc 保持原位置，新下载的 crates 和 registry 统一存到 E 盘工作区。
$env:CARGO_HOME = Join-Path $DependencyRoot 'cargo'
Write-Host "Initialized $Compiler; dependency cache: $DependencyRoot"
