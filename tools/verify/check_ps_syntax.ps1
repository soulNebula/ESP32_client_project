# PowerShell
# 查 PowerShell 语法
# 语法检查小工具：解析指定 .ps1 文件，报告语法错误行号
# 用法: powershell -File tools\verify\check_ps_syntax.ps1 -Path tools\build\build.ps1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string[]]$MorePaths
)

$all = @($Path) + @($MorePaths)

# PowerShell 5.1 把逗号分隔的多值参数传给 [string[]] 时，可能整体当一个字符串，
# 这里兜一层手动拆分，保证 "-MorePaths a,b" 和 "-MorePaths a -MorePaths b" 都能用。
$flat = @()
foreach ($item in $all) {
    if ($item -like '*,*') { $flat += ($item -split ',') } else { $flat += $item }
}
$all = $flat | Where-Object { $_ -and $_.Trim() -ne '' } | ForEach-Object { $_.Trim() }

$bad = 0

foreach ($p in $all) {
    if (-not (Test-Path $p)) {
        Write-Host ("[SKIP] not found: {0}" -f $p) -ForegroundColor Yellow
        continue
    }
    $errs = $null
    $null = [System.Management.Automation.Language.Parser]::ParseFile(
                (Resolve-Path $p).Path, [ref]$null, [ref]$errs)
    if ($errs -and $errs.Count -gt 0) {
        $bad++
        Write-Host ("[FAIL] {0}" -f $p) -ForegroundColor Red
        $errs | ForEach-Object {
            Write-Host ("   LINE {0}: {1}" -f $_.Extent.StartLineNumber, $_.Message) -ForegroundColor Red
        }
    } else {
        Write-Host ("[ OK ] {0}" -f $p) -ForegroundColor Green
    }
}

if ($bad -gt 0) { exit 1 }
exit 0
