# PowerShell
# 字库覆盖自检
#
# 屏上的中文必须在字模里有，否则显示成空心方框（"豆腐块"）。
# 本脚本把源码里出现的汉字和 ui/panel_font_data.h 里的码点对一遍。
#
#   powershell -ExecutionPolicy Bypass -File arduino\SmartHomeArduino\tools\check_font.ps1
#
# 缺字就重跑生成器（先刷新字符集再出表）：
#   python arduino\SmartHomeArduino\tools\gen_font.py --scan

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

$fontFile = Join-Path $Sketch 'panel_font_data.h'

Write-Host ''
Write-Host '=== 字库覆盖自检 ===' -ForegroundColor Cyan
Write-Host "  sketch : $Sketch"
Write-Host ''

if (-not (Test-Path -LiteralPath $fontFile)) {
    Write-Host "  [FAIL] 找不到 $fontFile" -ForegroundColor Red
    Write-Host '         跑一次： python tools\gen_font.py --scan'
    Write-Host ''
    exit 1
}

# 1. 字模里有哪些码点
$fontText = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($fontFile))
$codes = New-Object 'System.Collections.Generic.HashSet[int]'
foreach ($m in [regex]::Matches($fontText, '\{0x([0-9A-Fa-f]{4}),')) {
    [void]$codes.Add([Convert]::ToInt32($m.Groups[1].Value, 16))
}

# 源码里的字和字符串里的字分开统计
$srcChars = New-Object 'System.Collections.Generic.HashSet[int]'
$strChars = New-Object 'System.Collections.Generic.HashSet[int]'

$files = Get-ChildItem -LiteralPath $Sketch -Recurse -File |
    Where-Object { $_.Extension -in @('.c', '.cpp', '.h', '.ino') } |
    Where-Object { $_.FullName -notmatch '\\tools\\' -and $_.Name -ne 'panel_font_data.h' }

foreach ($f in $files) {
    $t = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($f.FullName))

    foreach ($ch in $t.ToCharArray()) {
        $c = [int][char]$ch
        # 汉字 + ° + ℃
        if (($c -ge 0x4E00 -and $c -le 0x9FFF) -or $c -eq 0x00B0 -or $c -eq 0x2103) {
            [void]$srcChars.Add($c)
        }
    }

    # 字符串字面量里的（屏上真会显示的）
    foreach ($m in [regex]::Matches($t, '"((?:[^"\\]|\\.)*)"')) {
        foreach ($ch in $m.Groups[1].Value.ToCharArray()) {
            $c = [int][char]$ch
            if (($c -ge 0x4E00 -and $c -le 0x9FFF) -or $c -eq 0x00B0 -or $c -eq 0x2103) {
                [void]$strChars.Add($c)
            }
        }
    }
}

# 2. 比对
$missSrc = @($srcChars | Where-Object { -not $codes.Contains($_) } | Sort-Object)
$missStr = @($strChars | Where-Object { -not $codes.Contains($_) } | Sort-Object)

Write-Host "  字模码点数   : $($codes.Count)"
Write-Host "  源码用字     : $($srcChars.Count)"
Write-Host "  字符串用字   : $($strChars.Count)"
Write-Host ''

if ($missSrc.Count -eq 0) {
    Write-Host '  [OK] 源码里每个汉字都在字模里' -ForegroundColor Green
    Write-Host ''
    exit 0
}

# 只报字符串里的（那是真会显示成豆腐块的）
if ($missStr.Count -gt 0) {
    $s = -join ($missStr | ForEach-Object { [char]$_ })
    Write-Host "  [FAIL] 字符串里这些字没有字模，屏上会是空心方框：" -ForegroundColor Red
    Write-Host "         $s" -ForegroundColor Red
}
else {
    $s = -join ($missSrc | ForEach-Object { [char]$_ })
    Write-Host '  [WARN] 字符串都能显示，但注释里这些字没有字模：' -ForegroundColor Yellow
    Write-Host "         $s" -ForegroundColor Yellow
}

Write-Host ''
Write-Host '  修法： python tools\gen_font.py --scan' -ForegroundColor Cyan
Write-Host ''
exit 1
