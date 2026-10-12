# PowerShell
# 稳定烧录，绕开本板校验失败
#  flash.ps1 -- 稳定烧录脚本（绕开本机 CH343 串口的 MD5 校验失败）
#  ★ 为什么不能直接用 idf.py flash：
#    本机这块开发板的 USB 串口（CH343 桥）在【大量数据连续写入】时不稳定，
#    表现是 esptool 报：
#        A fatal error occurred: MD5 of file does not match data in flash!
#     或  A fatal error occurred: Failed to write to target Flash after seq N
#    此时【固件本身没问题】—— 是写入通道抖了。
#
#  稳定做法（本脚本做的三件事）：
#     1. 先 erase_region 把目标区域擦干净（这一步是稳定烧录的关键）；
#     2. 再 write_flash，并让 esptool 自带 --verify；
#     3. 最后独立 verify_flash 再核一遍，看到 "verify OK (digest matched)" 才算成功。
#
#  ★ 已知的另一处"看起来像失败其实正常"：
#    ota_data_initial.bin 的内容就是全 0xFF，和"擦干净的 flash"无法区分，
#    所以重烧同一块板时它【必然】报 MD5 不匹配。
#    bootloader 对此是容错的（会打
#    "ota data partition invalid and no factory, will try all partitions"
#    然后正常从 ota_0 启动），不影响运行。所以本脚本默认【跳过 otadata】，
#    需要时用 -IncludeOtadata 显式带上。
#
#  用法：
#    pwsh -File tools\build\flash.ps1                       # 烧 app + bootloader + 分区表 + 语音模型
#    pwsh -File tools\build\flash.ps1 -Port COM5
#    pwsh -File tools\build\flash.ps1 -AppOnly              # 只烧 app（改代码后最常用，最快；不含模型）
#    pwsh -File tools\build\flash.ps1 -SkipModel            # 显式不烧语音模型
#    pwsh -File tools\build\flash.ps1 -IncludeOtadata       # 连 otadata 一起写
#    pwsh -File tools\build\flash.ps1 -Baud 460800          # 通道好的时候可以提速
#
#  ★ 关于语音模型（model 分区，0x820000 / 4MB）：
#    语音识别模型不在 app 里，而是由 esp-sr 打包成 build\srmodels\srmodels.bin，
#    烧进独立的 model 分区。本脚本默认【连模型一起烧】—— 漏了它的现象只是
#    "喊了没反应"（日志会打 "esp_srmodel_init(model) 失败"），很难自己想到是少烧分区。
#    模型只在换唤醒词/命令词模型时才需要重烧，所以日常改代码用 -AppOnly 即可。
[CmdletBinding()]
param(
    [string]$Port = 'COM33',
    [int]$Baud = 115200,
    [switch]$AppOnly,
    [switch]$IncludeOtadata,
    [switch]$EraseAll,
    # 显式跳过 model 分区（语音模型）：默认会连 srmodels.bin 一起烧，
    # 因为漏了它语音识别会报 "model 分区里没有模型"，而现象只是"喊了没反应"。
    [switch]$SkipModel
)

$ErrorActionPreference = 'Stop'

$IdfPython = 'E:\Espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe'
# 本脚本在 tools/build/ 下，工程根 = 上两级目录
$repoRoot  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildDir  = Join-Path $repoRoot 'build'

if (-not (Test-Path $IdfPython)) { throw "IDF python not found: $IdfPython" }
if (-not (Test-Path (Join-Path $buildDir 'smart_home_s3.bin'))) {
    throw "固件不存在，请先构建： pwsh -File tools\build\idf_build.ps1"
}

Push-Location $buildDir
try {
    $esptool = @('-m', 'esptool', '--chip', 'esp32s3', '--port', $Port, '--baud', "$Baud")

    # ---- 0. 需要的话整片擦除（换分区表、固件起不来时用）----
    if ($EraseAll) {
        Write-Host '=== 整片擦除 erase_flash ===' -ForegroundColor Yellow
        & $IdfPython @esptool erase_flash
        if ($LASTEXITCODE -ne 0) { throw 'erase_flash failed' }
    }

    # ---- 1. 擦除 app 区域（4MB）----
    # 这一步是稳定烧录的【关键】：先擦干净再写，避免"写进去的和读出来的不一致"。
    Write-Host '=== 1/3 擦除 app 区域 (0x20000, 0x400000) ===' -ForegroundColor Cyan
    & $IdfPython @esptool erase_region 0x20000 0x400000
    if ($LASTEXITCODE -ne 0) { throw 'erase_region failed' }

    # ---- 1b. 语音模型（model 分区，0x820000 起 4MB）----
    # ★ 为什么必须在这里处理：esp-sr 的模型不在 app 里，而在独立的 model 分区。
    #   只烧 smart_home_s3.bin 的话，跑起来会报
    #     "esp_srmodel_init(model) 失败 —— model 分区里没有模型"
    #   而用户看到的现象只是"喊了没反应"，很难自己定位到"少烧了一个分区"。
    #   srmodels.bin 由构建生成（build/srmodels/srmodels.bin），已按分区大小补齐。
    $modelBin = Join-Path $buildDir 'srmodels\srmodels.bin'
    $modelOffset = '0x820000'
    $modelSize   = 0x400000
    $flashModel  = (-not $AppOnly) -and (-not $SkipModel) -and (Test-Path $modelBin)
    if ($flashModel) {
        Write-Host '=== 1b/3 擦除语音模型分区 (0x820000, 4MB) ===' -ForegroundColor Cyan
        & $IdfPython @esptool erase_region $modelOffset "$modelSize"
        if ($LASTEXITCODE -ne 0) { throw 'erase_region (model) failed' }
    } elseif (-not $AppOnly -and -not $SkipModel) {
        Write-Host "[WARN] 没找到 $modelBin —— 本次【不会】烧语音模型。" -ForegroundColor Yellow
        Write-Host '       请先完整构建（pwsh -File tools\build\idf_build.ps1），再重跑本脚本。' -ForegroundColor Yellow
    } elseif ($AppOnly -and -not $SkipModel) {
        Write-Host '[WARN] -AppOnly 只烧 app，【不含语音模型】。' -ForegroundColor Yellow
        Write-Host '       如果这块板还从没烧过模型，语音会报 "model 分区里没有模型"；' -ForegroundColor Yellow
        Write-Host '       跑一次不带 -AppOnly 的完整烧录即可（模型不用每次改代码都烧）。' -ForegroundColor Yellow
    }

    # ---- 2. 写入 ----
    # --verify 让 esptool 写完立刻校验（比后面单独跑 verify 更快发现问题）。
    Write-Host '=== 2/3 写入固件 ===' -ForegroundColor Cyan
    $targets = @()
    $targets += @('0x20000', 'smart_home_s3.bin')
    if (-not $AppOnly) {
        $targets += @('0x0',     'bootloader\bootloader.bin')
        $targets += @('0x8000',  'partition_table\partition-table.bin')
        if ($flashModel)    { $targets += @($modelOffset, 'srmodels\srmodels.bin') }
        if ($IncludeOtadata) { $targets += @('0xf000', 'ota_data_initial.bin') }
    }

    & $IdfPython @esptool --before default_reset --after hard_reset `
        write_flash --verify --flash_mode dio --flash_freq 80m --flash_size 16MB @targets
    if ($LASTEXITCODE -ne 0) { throw 'write_flash failed' }

    # ---- 3. 独立再校验一遍 ----
    # 单独 verify 的价值：write_flash 自带的 verify 万一被跳过或误判，
    # 这一步会以"从 flash 读回来算 digest"的方式独立确认。
    Write-Host '=== 3/3 独立校验 ===' -ForegroundColor Cyan
    & $IdfPython @esptool verify_flash --flash_mode dio --flash_size 16MB `
        '0x20000' 'smart_home_s3.bin'
    if ($LASTEXITCODE -ne 0) { throw 'verify_flash failed' }

    Write-Host ''
    Write-Host '[ OK ] 烧录并校验通过（看到 verify OK (digest matched) 才算真成功）' -ForegroundColor Green
}
finally {
    Pop-Location
}
