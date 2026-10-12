# PowerShell
# 等源文件齐了再构建
#  wait_sources.ps1 —— 等所有组件源文件就位（并行开发的收口工具）
#  用途：几个 Agent 并行写不同组件，本脚本轮询直到"清单里的文件全部存在"，
#        然后立刻启动一次构建。这样集成阶段不用人工盯着。
#
#  用法：
#    powershell -File tools\build\wait_sources.ps1
#    powershell -File tools\build\wait_sources.ps1 -TimeoutSec 1800
#    powershell -File tools\build\wait_sources.ps1 -NoBuild      # 只等，不构建
[CmdletBinding()]
param(
    [int]$TimeoutSec = 1800,
    [switch]$NoBuild,
    [int]$PollSec = 5
)

$ErrorActionPreference = 'Continue'
# 本脚本在 tools/build/ 下，工程根 = 上两级目录
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

# 期望存在的源文件清单（相对工程根目录）
$expected = @(
    'components\BSP\I2C\i2c_bus.c',
    'components\BSP\ADC\adc_bus.c',
    'components\BSP\GPIO\gpio_util.c',
    'components\BSP\LED\led.c',
    'components\BSP\SERVO\servo.c',
    'components\BSP\BUZZER\buzzer.c',
    'components\BSP\KEY\key.c',
    'components\BSP\SENSOR\sensor.c',
    'components\BSP\VOICE\voice.c',
    'components\ui\panel.c',
    'components\ui\panel_font.c',
    'components\ui\font_cn_data.c',
    'components\ui\font_cn_extra.c',
    'components\ui\display_view.c',
    'components\App\device_model.c',
    'components\App\app_link.c',
    'components\App\automation.c',
    'components\App\home_auto.c',
    'components\App\net_link.c',
    'components\App\mqtt_protocol.c',
    'main\main.c',
    'main\board.c'
)

Write-Host ''
Write-Host '=== waiting for all component sources ===' -ForegroundColor Cyan

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$reported = @{}

while ($true) {
    $missing = @()
    foreach ($rel in $expected) {
        $full = Join-Path $repoRoot $rel
        if (-not (Test-Path $full)) { $missing += $rel }
    }

    if ($missing.Count -eq 0) {
        Write-Host ''
        Write-Host ("[OK] all {0} sources present" -f $expected.Count) -ForegroundColor Green
        break
    }

    # 只在"缺失集合发生变化"时打印，避免刷屏
    $key = ($missing -join '|')
    if (-not $reported.ContainsKey($key)) {
        $reported[$key] = $true
        Write-Host ("  waiting {0} file(s): {1}" -f $missing.Count, ($missing -join ', ')) -ForegroundColor Yellow
    }

    if ((Get-Date) -gt $deadline) {
        Write-Host ''
        Write-Host ("[TIMEOUT] still missing {0} file(s):" -f $missing.Count) -ForegroundColor Red
        $missing | ForEach-Object { Write-Host ("   - " + $_) -ForegroundColor Red }
        exit 2
    }

    Start-Sleep -Seconds $PollSec
}

if ($NoBuild) {
    Write-Host 'NoBuild specified - exiting.' -ForegroundColor Gray
    exit 0
}

Write-Host ''
Write-Host '=== starting build ===' -ForegroundColor Cyan
& (Join-Path $PSScriptRoot 'build.ps1') @args
exit $LASTEXITCODE
