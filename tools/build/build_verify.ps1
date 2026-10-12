# PowerShell
# 隔离构建，产物进 build_verify
#  build_verify.ps1 -- 隔离构建入口（把 -B 放在子命令之前，编译产物进独立的 build_verify 目录）
#  为什么需要它：
#    · 与默认的 build/ 隔离，避免多个构建互相踩 CMake/ninja 缓存。
#    · -B 是 idf.py 的【全局】选项，必须在子命令**之前**，
#      顺序拼错会报 "Error: No such option: -B"（exit 2）。
#
#  用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools\build\build_verify.ps1
$ErrorActionPreference = 'Continue'

$IdfPath   = 'E:\Espressif\frameworks\esp-idf-v5.4.4'
$IdfPython = 'E:\Espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe'
$buildDir  = 'build_verify'

# 本脚本在 tools/build/ 下，工程根 = 上两级目录
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $repoRoot

# 中文注释 + GBK 默认编码会让 esp-sr 打包模型那一步 UnicodeDecodeError
$env:PYTHONUTF8       = '1'
$env:PYTHONIOENCODING = 'utf-8'

Write-Host "=== activating ESP-IDF: $IdfPath ===" -ForegroundColor Cyan
$export = & $IdfPython "$IdfPath\tools\activate.py" --export 2>$null | Select-Object -Last 1
if (-not $export) { Write-Host '[FAIL] 拿不到 IDF export 脚本' -ForegroundColor Red; exit 1 }
. $export

Write-Host "=== idf.py -B $buildDir build ===" -ForegroundColor Cyan
& idf.py -B $buildDir build
$code = $LASTEXITCODE

Pop-Location
Write-Host "=== EXITCODE=$code ===" -ForegroundColor $(if ($code -eq 0) { 'Green' } else { 'Red' })
exit $code
