# PowerShell
# 结构自检（改完代码先跑这个）
#
# 查这些：
#   · 每个文件头两行是不是 `// c` + 一句中文说明
#   · 有没有 Tab、有没有 BOM、行尾是不是 LF
#   · #include "xxx.h" 能不能在工程里找到
#   · 头文件里声明的函数，有没有对应实现
#   · 注释里有没有 `/* ==== */` 分隔线或 `@param` 那一套
#
#   powershell -ExecutionPolicy Bypass -File arduino\SmartHomeArduino\tools\check_style.ps1

[CmdletBinding()]
param(
    [string]$Sketch = ''
)

$ErrorActionPreference = 'Stop'

if (-not $Sketch) {
    # 脚本在 tools\ 下，工程根 = 上一级
    $here = Split-Path -Parent $MyInvocation.MyCommand.Path
    $Sketch = (Get-Item (Join-Path $here '..')).FullName
}

$LF = [char]10
$problems = New-Object System.Collections.Generic.List[string]

Write-Host ''
Write-Host '=== Arduino 工程结构自检 ===' -ForegroundColor Cyan
Write-Host "  sketch : $Sketch"
Write-Host ''

$files = Get-ChildItem -LiteralPath $Sketch -Recurse -File |
    Where-Object { $_.Extension -in @('.c', '.cpp', '.h', '.ino') } |
    Where-Object { $_.FullName -notmatch '\\tools\\' } |
    Sort-Object FullName

# 生成的字模表不参与风格检查
$skipped = @('panel_font_data.h')

# 外部头文件（核心与 IDF 带的），不查工程里有没有
$externHeaders = @(
    'Arduino.h', 'WiFi.h', 'Preferences.h', 'BLE.h', 'BLEDevice.h', 'BLEServer.h',
    'BLEUtils.h', 'BLE2902.h', 'ESP_SR.h', 'ESP_I2S.h', 'esp32-hal-rmt.h',
    'esp_err.h', 'esp_log.h', 'esp_timer.h', 'esp_rom_sys.h', 'esp_system.h',
    'esp_heap_caps.h', 'nvs.h', 'nvs_flash.h', 'soc/soc_caps.h', 'sdkconfig.h',
    'freertos/FreeRTOS.h', 'freertos/task.h', 'freertos/semphr.h', 'freertos/queue.h',
    'driver/gpio.h', 'driver/ledc.h', 'driver/i2c.h', 'driver/i2s.h',
    'driver/rmt_tx.h', 'driver/rmt_encoder.h', 'driver/rmt_rx.h',
    'hal/gpio_types.h', 'esp_adc/adc_oneshot.h', 'esp_adc/adc_cali.h',
    'esp_adc/adc_cali_scheme.h', 'esp_private/esp_clk.h'
)

$checked = 0

foreach ($f in $files) {
    $checked++
    $rel = $f.FullName.Substring($Sketch.Length).TrimStart('\')

    # 1. UTF-8 无 BOM
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $problems.Add("$rel : 有 UTF-8 BOM（本工程要求【无 BOM】）")
    }

    # 2. 行尾统一 LF
    if ($bytes -contains 0x0D) {
        $problems.Add("$rel : 有 CR（行尾应是 LF）")
    }

    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    $lines = $text -split $LF

    # 3. 头两行：`// c` + 中文说明
    if ($lines.Count -lt 2) {
        $problems.Add("$rel : 文件太短，缺少头部注释")
    }
    else {
        if ($lines[0].Trim() -ne '// c') {
            $problems.Add("$rel : 第 1 行必须是 '// c'，现在是 '$($lines[0].Trim())'")
        }
        $second = $lines[1].Trim()
        if (-not $second.StartsWith('//')) {
            $problems.Add("$rel : 第 2 行必须是一句中文说明的注释")
        }
        elseif ($second -notmatch '[\u4e00-\u9fff]') {
            $problems.Add("$rel : 第 2 行没有中文说明")
        }
    }

    # 4. 不许 Tab
    $ln = 0
    foreach ($line in $lines) {
        $ln++
        if ($line.Contains([char]9)) {
            $problems.Add("$rel : 第 $ln 行有 Tab（用 4 空格）")
        }
    }

    # 5. 注释里别有分隔线 / javadoc
    #    单行注释太长也报（本工程注释要短）
    $ln = 0
    foreach ($line in $lines) {
        $ln++
        $t = $line.Trim()
        if ($t.StartsWith('//') -or $t.StartsWith('*') -or $t.StartsWith('/*')) {
            if ($t -match '=====' -or $t -match '-----' -or $t -match '@param' -or $t -match '@return' -or $t -match '@brief') {
                $problems.Add("$rel : 第 $ln 行注释用了分隔线或 javadoc：$t")
            }

            # 注释里的汉字个数（去掉 // 与空白）
            $body = $t -replace '^//+', '' -replace '^\*+', '' -replace '^/\*+', ''
            $body = $body.Trim()
            $cjk = ([regex]::Matches($body, '[\u4e00-\u9fff]')).Count
            if ($cjk -gt 30) {
                $problems.Add("$rel : 第 $ln 行注释太长（$cjk 个汉字）：$body")
            }
        }
    }

    # 6. 本地 #include 都要能找到
    $ln = 0
    foreach ($line in $lines) {
        $ln++
        if ($line -match '^\s*#\s*include\s+"([^"]+)"') {
            $inc = $Matches[1]
            if ($externHeaders -contains $inc) { continue }
            $cands = @(
                (Join-Path $f.DirectoryName $inc),
                (Join-Path $Sketch $inc),
                (Join-Path (Join-Path $Sketch 'bsp') $inc),
                (Join-Path (Join-Path $Sketch 'ui') $inc),
                (Join-Path (Join-Path $Sketch 'tools') $inc)
            )
            $hit = $false
            foreach ($c in $cands) { if (Test-Path -LiteralPath $c) { $hit = $true; break } }
            if (-not $hit) {
                $problems.Add("$rel : 第 $ln 行 #include ""$inc"" 在工程里找不到")
            }
        }
    }
}

# 7. 头文件里声明的函数要有实现
$headers = $files | Where-Object { $_.Extension -eq '.h' }
$implSrc = ($files | Where-Object { $_.Extension -in @('.cpp', '.ino') } |
    ForEach-Object { [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($_.FullName)) }) -join $LF

foreach ($h in $headers) {
    $rel = $h.FullName.Substring($Sketch.Length).TrimStart('\')
    $htext = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($h.FullName))

    # 只挑形如 `app_err_t foo(...);` 的声明
    $ms = [regex]::Matches($htext, '(?m)^\s*(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\([^;{)]*\)\s*;')
    foreach ($m in $ms) {
        $name = $m.Groups[1].Value
        # 跳过明显的宏与类型
        if ($name -in @('if', 'while', 'for', 'switch', 'return', 'sizeof', 'template', 'class', 'struct', 'enum')) { continue }
        if ($implSrc -notmatch ("\b" + [regex]::Escape($name) + "\s*\(")) {
            $problems.Add("$rel : 声明了 $name() 但工程里找不到实现")
        }
    }
}

Write-Host "  检查了 $checked 个源文件（跳过：$($skipped -join ', ')）" -ForegroundColor DarkGray
Write-Host ''

if ($problems.Count -eq 0) {
    Write-Host '  [OK] 结构自检全部通过' -ForegroundColor Green
    Write-Host ''
    exit 0
}

foreach ($p in $problems) {
    Write-Host "  [PROBLEM] $p" -ForegroundColor Red
}
Write-Host ''
Write-Host "  [FAIL] $($problems.Count) 个问题" -ForegroundColor Red
Write-Host ''
exit 1
