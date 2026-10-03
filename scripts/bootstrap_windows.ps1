<#
.SYNOPSIS
下载并校验 Windows 构建依赖，后续构建可以完全离线。
.DESCRIPTION
所有软件包、工具链与 Cargo 下载缓存集中保存到工作区相邻的 faio-deps；
归档版本、提交和 SHA256 由 windows-dependencies.json 固定，不读取 latest。
#>
[CmdletBinding()]
param([string]$DependencyRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '..\faio-deps'), [switch]$Offline, [switch]$SkipMinGW)
$ErrorActionPreference = 'Stop'
$DependencyRoot = [IO.Path]::GetFullPath($DependencyRoot)
$manifest = Get-Content (Join-Path $PSScriptRoot 'windows-dependencies.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path "$DependencyRoot\archives", "$DependencyRoot\src", "$DependencyRoot\tools", "$DependencyRoot\cargo" | Out-Null
foreach ($name in @('spdlog', 'googletest', 'asio', 'mingw')) {
    if ($name -eq 'mingw' -and $SkipMinGW) { continue }
    $item = $manifest.$name
    if ($name -eq 'mingw') {
        # 验证矩阵只支持固定的 MSYS2 native TLS 工具链；不再保留旧 WinLibs 分支。
        if ($item.kind -ne 'msys2-native-tls') { throw "Unsupported MinGW manifest kind: $($item.kind)" }
        # GCC native TLS、CRT、libstdc++、libgcc 和 pthread 来自同一固定发行版。
        # 只解压原生 ucrt64 包，无需安装 MSYS shell 或修改系统 PATH。
        $packages = Get-Content (Join-Path $PSScriptRoot $item.packages) -Raw | ConvertFrom-Json
        $toolchainRoot = Join-Path "$DependencyRoot\tools" $item.install_subdir
        $markerRoot = Join-Path $toolchainRoot '.faio-packages'
        New-Item -ItemType Directory -Force -Path $toolchainRoot, $markerRoot | Out-Null
        foreach ($package in $packages) {
            $packageArchive = Join-Path "$DependencyRoot\archives" $package.filename
            if (-not (Test-Path -LiteralPath $packageArchive)) {
                if ($Offline) { throw "Offline MSYS2 archive missing: $packageArchive" }
                Write-Host "Downloading $($package.name) $($package.version)"
                Invoke-WebRequest -Uri $package.url -OutFile $packageArchive
            }
            $packageHash = (Get-FileHash -LiteralPath $packageArchive -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($packageHash -ne $package.sha256) { throw "SHA256 mismatch for $packageArchive" }
            $marker = Join-Path $markerRoot "$($package.name).sha256"
            if (-not (Test-Path -LiteralPath $marker) -or (Get-Content -LiteralPath $marker -Raw).Trim() -ne $packageHash) {
                & tar -xf $packageArchive -C $toolchainRoot --exclude=.PKGINFO --exclude=.MTREE --exclude=.BUILDINFO
                if ($LASTEXITCODE -ne 0) { throw "Failed to unpack $packageArchive" }
                Set-Content -LiteralPath $marker -Value $packageHash -Encoding ascii
            }
        }
        continue
    }
    # 剩余三项是测试/benchmark 使用的固定版本第三方源码，均采用 tar.gz 归档。
    $archive = Join-Path "$DependencyRoot\archives" "$name.tar.gz"
    if (-not (Test-Path -LiteralPath $archive)) {
        if ($Offline) { throw "Offline dependency archive missing: $archive" }
        Write-Host "Downloading $name $($item.version)"
        Invoke-WebRequest -Uri $item.url -OutFile $archive
    }
    $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $item.sha256) { throw "SHA256 mismatch for $archive" }
    $source = Join-Path "$DependencyRoot\src" $name
    if (-not (Test-Path -LiteralPath $source)) {
        New-Item -ItemType Directory -Force -Path $source | Out-Null
        & tar -xf $archive -C $source --strip-components=1
        if ($LASTEXITCODE -ne 0) { throw "Failed to unpack $archive" }
    }
}
Write-Host "Verified offline dependency cache: $DependencyRoot"
