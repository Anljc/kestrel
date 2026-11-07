# 一键跑测试 + 基准（Windows 侧入口）。
#
# 真正的基准流程写在 run_all.sh 里，这里只负责把它送进 WSL：
#   - 工程本身是 POSIX 优先的（Env 的 Win32 分支尚未实现），
#     基准必须在 WSL/Linux 下跑才有意义；
#   - 参数走环境变量透传，避免再维护一份 PS 版的参数解析。
#
# 用法：
#   powershell -File benches\run_all.ps1
#   $env:OPS=200000; powershell -File benches\run_all.ps1
#   $env:SKIP_BUILD=1; powershell -File benches\run_all.ps1   # 只重跑基准
param(
  [string]$Distro = "Ubuntu"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
# WSL 里访问 Windows 路径要换成 /mnt/<盘符>/...
$wslRoot = "/mnt/" + $root.Substring(0, 1).ToLower() + $root.Substring(2).Replace('\', '/')

Write-Host "==> WSL($Distro): $wslRoot/benches/run_all.sh"

# 把关心的环境变量显式带过去（WSL 默认不会继承 Windows 的环境变量）
$passthrough = @("OPS", "READ_OPS", "BENCH_DIR", "OUT_CSV", "SKIP_BUILD", "SKIP_TEST", "BUILD_DIR")
$exports = ($passthrough | Where-Object { Test-Path "env:$_" } | ForEach-Object {
    $value = [Environment]::GetEnvironmentVariable($_)
    "export $_='$value'"
}) -join "; "

$script = "cd '$wslRoot' && $exports && bash benches/run_all.sh"
wsl -d $Distro -e bash -lc $script
exit $LASTEXITCODE
