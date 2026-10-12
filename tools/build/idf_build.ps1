# PowerShell
# 直调 idf.py，抓出真实错误
#  idf_build.ps1 -- 直接调 idf.py 构建/烧录，并把【真正的错误】抓出来
#  为什么需要这个脚本（而不是直接用 tools/build/build.ps1）：
#    build.ps1 用的是 cmd 重定向，ninja 失败时真正的编译错误会落在
#    build/log/idf_py_stderr_output_*.txt 里，而汇总日志只留一句
#    "ninja failed with exit code 1"，看不到是哪一行、什么错。
#    本脚本把 idf.py 的输出原样接出来，只把含 error / undefined / FAILED 的
#    行筛出来打印，集成排错时一眼就能定位。
#
#  用法：
#    powershell -File tools\build\idf_build.ps1                 # build
#    powershell -File tools\build\idf_build.ps1 -Task flash     # flash
#    powershell -File tools\build\idf_build.ps1 -Task fullclean
#    powershell -File tools\build\idf_build.ps1 -Full           # 打印完整输出（不筛）
[CmdletBinding()]
param(
    [string]$Task = 'build',
    [string]$Port = '',
    # ★ 烧录波特率先给 115200：本机这块板的 CH343 + 长杜邦线在高速下会
    #   "MD5 of file does not match data in flash"（见 docs 里的烧录注意事项）。
    #   稳定优先，115200 慢一点但一次过；想快可以 -Baud 460800 试。
    [int]$Baud = 115200,
    [int]$Jobs = 0,
    [switch]$Full,
    # 跳过"flash 前预擦除 app 分区"（见下方说明；只是想重烧同一份固件时可用）
    [switch]$SkipErase
)

$ErrorActionPreference = 'Continue'

$IdfPath   = 'E:\Espressif\frameworks\esp-idf-v5.4.4'
$IdfPython = 'E:\Espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe'
if (-not (Test-Path $IdfPython)) { $IdfPython = Join-Path $IdfPath 'tools\idf.py' }

# 本脚本在 tools/build/ 下，工程根 = 上两级目录
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $repoRoot

# ★ 必须让 Python 按 UTF-8 读文件，否则构建会在【打包语音模型】这一步挂掉：
#   esp-sr 的 model/movemodel.py 用 io.open(sdkconfig) 且【不指定 encoding】，
#   Windows 中文环境下默认走 GBK；而本工程的 sdkconfig 里有中文注释（UTF-8），
#   于是报 UnicodeDecodeError: 'gbk' codec can't decode byte ... 
#   现象是 ninja 报 `FAILED: srmodels/srmodels.bin`，看起来像模型问题，
#   其实只是编码问题。build.ps1 里早就设了这两个变量，这里补齐保持一致。
$env:PYTHONUTF8       = '1'
$env:PYTHONIOENCODING = 'utf-8'

try {
    if (-not (Test-Path $IdfPython)) {
        throw "IDF python env not found: $IdfPython"
    }

    Write-Host "=== activating ESP-IDF: $IdfPath ===" -ForegroundColor Cyan
    $export = & $IdfPython "$IdfPath\tools\activate.py" --export 2>$null | Select-Object -Last 1
    if (-not $export) { throw 'failed to obtain IDF export script' }
    . $export

    $cmd = if ($Task -eq 'monitor') { 'monitor' } else { $Task }
    $idfArgs = @($cmd)
    if ($Port) { $idfArgs += @('-p', $Port) }
    # 只有真正的烧录动作才需要 -b（build/fullclean 传了反而报错）
    if ($Port -and ($cmd -match 'flash|monitor')) { $idfArgs += @('-b', "$Baud") }
    if ($Jobs -gt 0 -and $Task -eq 'build') { $idfArgs += @('-j', "$Jobs") }

    Write-Host ("=== idf.py {0} ===" -f ($idfArgs -join ' ')) -ForegroundColor Cyan
    Write-Host ''

    # ★ 本机这块板子的已知坑：直接 flash 大概率报
    #     "A fatal error occurred: MD5 of file does not match data in flash!"
    #   实测定位过程（2026-10，COM33 / CH343 / GD25Q128 16MB）：
    #     · 不是压缩、不是波特率、不是杜邦线噪声 —— 115200/460800、
    #       带压缩/--no-compress 全部一样失败，而且失败位置完全固定：
    #       0x20000~0x13FFFF 这几个 64KB 扇区写进去读出来是错的，
    #       0x0~0x1FFFF 和 0x140000 以上却完全正常。
    #     · 根因是这段扇区处于"写/擦保护"的残留状态：整片擦除
    #       (erase_flash) 或把 app 分区 erase_region 一遍之后就恢复正常，
    #       所以真正的修法是"先擦再写"，而不是反复重试。
    #   因此这里在 flash 之前自动擦一遍 app 分区（0x20000，长度 4MB）。
    #   代价约 10 秒，换来的是不再出现"写到一半 MD5 对不上"。
    #   想跳过（例如只是重烧一次没改过的固件）加 -SkipErase。
    if ($cmd -match 'flash' -and $Port -and -not $SkipErase) {
        Write-Host '=== 预擦除 app 分区（规避本板 flash 扇区保护残留） ===' -ForegroundColor Cyan
        $esp = Join-Path $IdfPath 'components\esptool_py\esptool\esptool.py'
        & $IdfPython $esp --chip esp32s3 -p $Port -b 460800 erase_region 0x20000 0x400000 2>&1 |
            ForEach-Object { if ("$_" -match 'Erasing|completed|A fatal|error') { Write-Host "  $_" } }
        Write-Host ''
    }

    # 用管道接住全部输出；PowerShell 的管道是它能看到的真实 stdout/stderr
    & idf.py @idfArgs 2>&1 | ForEach-Object {
        $line = "$_"
        if ($Full) {
            Write-Host $line
        } elseif ($line -match 'error:|Error |error |undefined reference|FAILED|ninja:|fatal error|\*\*\* ') {
            # 编译错误优先红色；其它醒目提示黄色
            if ($line -match 'error:|undefined reference|fatal error') {
                Write-Host $line -ForegroundColor Red
            } else {
                Write-Host $line -ForegroundColor Yellow
            }
        } elseif ($line -match 'Project build complete|Successfully created|Generated |Bootloader binary size|Total image size') {
            Write-Host $line -ForegroundColor Green
        }
    }

    $code = $LASTEXITCODE
    Write-Host ''
    if ($code -eq 0) {
        Write-Host ("[ OK ] idf.py {0} succeeded" -f $cmd) -ForegroundColor Green
    } else {
        Write-Host ("[FAIL] idf.py {0} exit {1}" -f $cmd, $code) -ForegroundColor Red
        Write-Host '       完整输出请加 -Full 再跑一次，或看 build\log\ 下的文件。' -ForegroundColor Yellow
    }
    exit $code
}
finally {
    Pop-Location
}
