# PowerShell
# 生成补充汉字点阵
#  gen_panel_font.ps1 -- 生成 components/ui/font_cn_extra.c
#  作用：
#    把 tools/dev/font_charset.txt 里【基础表没有的】汉字渲染成 16x16 点阵，
#    输出成 panel_glyph_t 数组，供 panel_font_cn_lookup() 二分查找。
#
#  ★ 为什么要有这个脚本：
#    基础 101 字的点阵来自参考工程（SimHei 17pt 取模），是不可再生的资产。
#    UI 还需要若干基础表里没有的字（例如"温度湿度天气"里的"湿""气"），
#    这些字必须用【同一个字体、同一个字号(17pt)】渲染，否则字重和大小不一致，
#    混排时会一眼看出"有的字大有的字小"。
#
#  ★ 本脚本刻意【全部使用 ASCII】：
#    Windows PowerShell 读无 BOM 的 UTF-8 .ps1 时会按 ANSI/GBK 解析，
#    脚本里的中文字面量会被破坏、甚至导致语法错误。
#    所以汉字只写在 tools/dev/font_charset.txt 里，由脚本显式按 UTF-8 读回来。
#
#  格式契约（必须与 panel_font.h 一致）：
#    逐列式 / 纵向取模。每字节 = 同一列上连续 8 个像素，bit0 = 该列最上面一行。
#    先从左到右排完所有列，再排下一页。
#      中文 16x16 : 16 列 x 16 行 -> 32 字节（2 页）
#                    字节  0..15 = 上页（第 0~7 行）
#                    字节 16..31 = 下页（第 8~15 行）
#
#  用法：
#    pwsh -File tools/dev/gen_panel_font.ps1
#    pwsh -File tools/dev/gen_panel_font.ps1 -Preview      # 顺便打印点阵图案
#    pwsh -File tools/dev/gen_panel_font.ps1 -All          # 重新渲染字符集里全部字
[CmdletBinding()]
param(
    [switch]$Preview,
    # 默认只生成"基础表里没有的"；-All 则把字符集里的字全部重新渲染。
    [switch]$All
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# 本脚本在 tools/dev/ 下，工程根 = 上两级目录
$repoRoot    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outFile     = Join-Path $repoRoot 'components\ui\font_cn_extra.c'
$baseFile    = Join-Path $repoRoot 'components\ui\font_cn_data.c'
$charsetFile = Join-Path $PSScriptRoot 'font_charset.txt'

$ArrayName  = 'panel_cn_extra'
$CountName  = 'panel_cn_extra_count'
$CnFontName = 'SimHei'
$CellW = 16
$CellH = 16
# 从 17 开始试：基础表就是 17pt 生成的，必须一致。
$EmCandidates = @(17, 16, 15, 14, 13, 12, 11, 10)

#  辅助函数

function New-Canvas([int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Black)
    # SingleBitPerPixelGridFit = 不做抗锯齿，纯黑白，正是 1bpp 点阵字库需要的。
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
    $g.PageUnit = [System.Drawing.GraphicsUnit]::Pixel
    return @{ bmp = $bmp; g = $g }
}

function Get-InkBBox($bmp, [int]$w, [int]$h) {
    $minX = $w; $minY = $h; $maxX = -1; $maxY = -1
    for ($y = 0; $y -lt $h; $y++) {
        for ($x = 0; $x -lt $w; $x++) {
            if ($bmp.GetPixel($x, $y).R -gt 127) {
                if ($x -lt $minX) { $minX = $x }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }
                if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }
    if ($maxX -lt 0) { return $null }
    return @{ minX = $minX; minY = $minY; maxX = $maxX; maxY = $maxY }
}

# 把一块矩形导出成"逐列式"字节流
function Export-PageBytes($bmp, [int]$srcX, [int]$srcY, [int]$cols, [int]$rows) {
    $out = New-Object 'System.Collections.Generic.List[byte]'
    $pages = [int][math]::Ceiling($rows / 8.0)
    for ($p = 0; $p -lt $pages; $p++) {
        for ($c = 0; $c -lt $cols; $c++) {
            $b = 0
            for ($r = 0; $r -lt 8; $r++) {
                $y = $srcY + $p * 8 + $r
                if ($y -lt ($srcY + $rows)) {
                    if ($bmp.GetPixel($srcX + $c, $y).R -gt 127) {
                        $b = $b -bor (1 -shl $r)
                    }
                }
            }
            $out.Add([byte]$b)
        }
    }
    return $out.ToArray()
}

function Show-Preview([byte[]]$bytes, [int]$cols, [int]$rows, [string]$label) {
    Write-Host ("  [{0}]  {1}x{2}  ({3} bytes)" -f $label, $cols, $rows, $bytes.Length)
    for ($y = 0; $y -lt $rows; $y++) {
        $page = [int][math]::Floor($y / 8)
        $bit  = $y % 8
        $line = '    '
        for ($c = 0; $c -lt $cols; $c++) {
            $b = $bytes[$page * $cols + $c]
            $line += $(if ((($b) -shr $bit) -band 1) { '#' } else { '.' })
        }
        Write-Host $line
    }
}

# 每个字单独渲染 -> 量墨迹外框 -> 在外框中心放进 16x16 格子里
function Build-CnFont([char[]]$chars, [int]$cellW, [int]$cellH, [int[]]$emCandidates) {
    $canvas = 64
    $pad    = 16

    foreach ($em in $emCandidates) {
        $cells = @{}
        $maxW = 0; $maxH = 0; $allOk = $true; $failReason = ''

        $font = New-Object System.Drawing.Font($CnFontName, $em,
                    [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)

        foreach ($ch in $chars) {
            $c = New-Canvas $canvas $canvas
            $c.g.DrawString([string]$ch, $font, [System.Drawing.Brushes]::White, $pad, $pad)
            $c.g.Dispose()

            $bb = Get-InkBBox $c.bmp $canvas $canvas
            if ($null -eq $bb) {
                $failReason = ('U+{0:X4} 没有渲染出任何墨迹（字体缺这个字？）' -f [int]$ch)
                $c.bmp.Dispose(); $allOk = $false; break
            }

            $iw = $bb.maxX - $bb.minX + 1
            $ih = $bb.maxY - $bb.minY + 1
            if ($iw -gt $maxW) { $maxW = $iw }
            if ($ih -gt $maxH) { $maxH = $ih }

            if ($iw -gt $cellW -or $ih -gt $cellH) {
                $failReason = ('U+{0:X4} 墨迹 {1}x{2} 超出 {3}x{4} (em={5})' -f `
                               [int]$ch, $iw, $ih, $cellW, $cellH, $em)
                $c.bmp.Dispose(); $allOk = $false; break
            }

            $ox = [int][math]::Floor(($cellW - $iw) / 2) - $bb.minX
            $oy = [int][math]::Floor(($cellH - $ih) / 2) - $bb.minY

            $c2 = New-Canvas $cellW $cellH
            for ($y = 0; $y -lt $ih; $y++) {
                for ($x = 0; $x -lt $iw; $x++) {
                    if ($c.bmp.GetPixel($bb.minX + $x, $bb.minY + $y).R -gt 127) {
                        $dx = $ox + $bb.minX + $x
                        $dy = $oy + $bb.minY + $y
                        if ($dx -ge 0 -and $dx -lt $cellW -and $dy -ge 0 -and $dy -lt $cellH) {
                            $c2.bmp.SetPixel($dx, $dy, [System.Drawing.Color]::White)
                        }
                    }
                }
            }
            $c2.g.Dispose()

            $key = [int]$ch
            $cells[$key] = (Export-PageBytes $c2.bmp 0 0 $cellW $cellH)
            $c2.bmp.Dispose()
            $c.bmp.Dispose()
        }

        $font.Dispose()

        if ($allOk) {
            return @{ ok = $true; em = $em; cells = $cells; maxW = $maxW; maxH = $maxH }
        }
        Write-Host ("    em={0}: {1} -- 换更小的字号重试" -f $em, $failReason)
    }
    return @{ ok = $false }
}

function Format-Bytes([byte[]]$bytes) {
    $parts = @()
    foreach ($b in $bytes) { $parts += ('0x{0:X2}' -f $b) }
    return ($parts -join ', ')
}

#  主流程
Write-Host ''
Write-Host '============== panel CJK extra font generation =============='

if (-not (Test-Path $charsetFile)) { throw "charset file not found: $charsetFile" }

$rawChars = (Get-Content -Path $charsetFile -Encoding UTF8 -Raw) -replace '\s', ''
# 保留两类字符：
#   1) CJK 统一表意文字（U+4E00 ~ U+9FFF）—— 汉字文案
#   2) 少量全角符号（U+00B0 °、U+2103 ℃）—— 温度单位
# 为什么必须过滤而不是照单全收：本文件顶部有中文注释，注释里会出现破折号、
# 逗号等标点，它们不需要点阵；而且个别全角标点的墨迹比 16x16 格子还宽，
# 会导致整个字符集被迫降字号（连带所有汉字都小一号），所以只白名单这两类。
#
# ★ 关于"°"：它不在 ASCII 8x16 表里（那张表只有 0x20~0x7E），
#   所以只能像汉字一样做成 16x16 点阵。它的墨迹很小（约 3x3 像素），
#   居中放进 16x16 格子里会显得偏高偏小 —— 画温度时把 y 坐标减 2、
#   或者干脆用 ASCII 的 "C" 配一个略大的数字，都能接受。
#   想要更漂亮就得给 ASCII 表单独加一个 8x16 的 "°"，那要改 base table，
#   本工程不动基础表（它是从参考工程整理来的点阵资产，改了就得整体重造）。
$charList = $rawChars.ToCharArray() |
            Where-Object {
                $c = [int]$_
                ($c -ge 0x4E00 -and $c -le 0x9FFF) -or   # CJK 汉字
                $c -eq 0x00B0 -or                        # ° 度符号
                $c -eq 0x2103                            # ℃ 摄氏度符号
            } |
            Sort-Object -Unique
Write-Host ("charset file : {0}" -f $charsetFile)
Write-Host ("charset glyphs: {0}" -f $charList.Count)
if ($charList.Count -eq 0) { throw 'charset is empty' }

# ---- 读基础表里已有的码点，避免重复渲染 ----
$baseCodes = @()
if (Test-Path $baseFile) {
    $bText = Get-Content -Path $baseFile -Raw -Encoding UTF8
    $baseCodes = @([regex]::Matches($bText, '\{\s*0x([0-9A-Fa-f]{4}),\s*\{') |
                   ForEach-Object { [Convert]::ToInt32($_.Groups[1].Value, 16) })
}
Write-Host ("base table   : {0} glyphs" -f $baseCodes.Count)

$todo = @()
foreach ($ch in $charList) {
    $code = [int]$ch
    if (-not $All -and ($baseCodes -contains $code)) { continue }
    $todo += $ch
}
$todo = $todo | Sort-Object { [int]$_ }
Write-Host ("to generate  : {0} glyphs" -f $todo.Count)

if ($todo.Count -gt 0) {
    $codes = $todo | ForEach-Object { [int]$_ }
    for ($i = 1; $i -lt $codes.Count; $i++) {
        if ($codes[$i] -le $codes[$i - 1]) { throw ('码点没有严格升序，位置 {0}' -f $i) }
    }
}

# ---- 渲染 ----
$cells = @{}
$em = 0
if ($todo.Count -gt 0) {
    Write-Host ('[1/2] 渲染中文 {0}x{1} ...' -f $CellW, $CellH)
    $fcn = Build-CnFont $todo $CellW $CellH $EmCandidates
    if (-not $fcn.ok) { throw '中文渲染失败：没有任何候选字号能装下所有字' }
    $cells = $fcn.cells
    $em = $fcn.em
    Write-Host ("      OK  em={0}  最大墨迹 {1}x{2}" -f $fcn.em, $fcn.maxW, $fcn.maxH)
} else {
    Write-Host '[1/2] 没有需要新渲染的字'
}

# ---- 写文件 ----
Write-Host '[2/2] 写出 C 源文件 ...'
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('// c')
[void]$sb.AppendLine('// 补充字模数据（自动生成，别手改）')
[void]$sb.AppendLine('// 17pt 同源')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#include "panel_font.h"')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('// 每字 32 字节')
[void]$sb.AppendLine(('const panel_glyph_t {0}[] = {{' -f $ArrayName))

foreach ($ch in $todo) {
    $k = [int]$ch
    [void]$sb.AppendLine(('    {{ 0x{0:X4}, {{ {1} }} }},  /* U+{0:X4} */' -f `
        $k, (Format-Bytes $cells[$k])))
}

[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine(('const int {0} = {1};' -f $CountName, $todo.Count))
[void]$sb.AppendLine('')

[System.IO.File]::WriteAllText($outFile, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))

# ---- 自检 ----
$written = Get-Content -Path $outFile -Raw -Encoding UTF8
$entries = [regex]::Matches($written, '\{\s*0x([0-9A-Fa-f]{4}),\s*\{([^}]*)\}\s*\}')
$badBytes = 0
$writtenCodes = @()
foreach ($m in $entries) {
    $writtenCodes += [Convert]::ToInt32($m.Groups[1].Value, 16)
    $n = ([regex]::Matches($m.Groups[2].Value, '0x[0-9A-Fa-f]{2}')).Count
    if ($n -ne 32) { $badBytes++ }
}
$ascending = $true
for ($i = 1; $i -lt $writtenCodes.Count; $i++) {
    if ($writtenCodes[$i] -le $writtenCodes[$i - 1]) { $ascending = $false }
}
Write-Host ''
Write-Host ("written      : {0}  ({1} bytes)" -f $outFile, (Get-Item $outFile).Length)
Write-Host ("entries      : {0}" -f $entries.Count)
Write-Host ("32-byte rows : {0} bad" -f $badBytes)
Write-Host ("ascending    : {0}" -f $ascending)
if ($badBytes -ne 0 -or -not $ascending -or $entries.Count -ne $todo.Count) {
    throw 'self-check FAILED'
}
Write-Host 'self-check   : OK'
Write-Host '============================================================='

if ($Preview) {
    foreach ($ch in $todo) {
        Show-Preview $cells[[int]$ch] 16 16 ('U+{0:X4}' -f [int]$ch)
    }
}
