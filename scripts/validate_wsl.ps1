# Windows 入口；apt 安装使用发行版 root，编译和回归使用发行版默认用户。
param(
    [ValidateSet('download','install','bootstrap','environment','configure','build','epoll','uring','examples','consumer','summary','verify')]
    [string]$Phase = 'verify',
    [string]$Distribution = 'Ubuntu',
    [ValidateRange(1,64)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path $PSScriptRoot 'validate_wsl_linux.sh'
$linuxPath = (& wsl.exe -d $Distribution --exec wslpath -a $scriptPath).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve validation script inside WSL' }
$wslArguments = @('-d', $Distribution)
if ($Phase -in @('download','install','bootstrap')) { $wslArguments += @('-u','root') }
$wslArguments += @('--exec','env',"FAIO_WSL_JOBS=$Jobs",'bash',$linuxPath,$Phase)
& wsl.exe @wslArguments
if ($LASTEXITCODE -ne 0) { throw "WSL phase $Phase failed with exit code $LASTEXITCODE" }
