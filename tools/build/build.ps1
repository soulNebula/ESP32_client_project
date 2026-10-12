# PowerShell
# 编译 / 烧录 / 看串口
#  build.ps1 -- one-stop build / flash / monitor helper for this project
#  PROJECT : ESP32-S3 智能家居（光影语音版）
#            main -> App -> BSP -> IDF,  pin truth = components/BSP/board_config.h
#
#  USAGE
#    pwsh -File tools\build\build.ps1                     # 编译（默认动作）
#    pwsh -File tools\build\build.ps1 -Build              # 编译
#    pwsh -File tools\build\build.ps1 -Flash -Monitor     # 烧录 + 串口监视
#    pwsh -File tools\build\build.ps1 -Monitor -Port COM5 # 只监视
#    pwsh -File tools\build\build.ps1 -Menuconfig         # 改 WiFi / MQTT 配置
#    pwsh -File tools\build\build.ps1 -SetTarget          # idf.py set-target esp32s3
#    pwsh -File tools\build\build.ps1 -Clean              # 删 build 目录
#    pwsh -File tools\build\build.ps1 -FullClean          # 彻底清理
#    pwsh -File tools\build\build.ps1 -Erase              # 擦除整片 Flash
#    pwsh -File tools\build\build.ps1 -Size               # 固件体积分析
#    pwsh -File tools\build\build.ps1 -DryRun             # 只打印将执行的 idf.py 命令
#    pwsh -File tools\build\build.ps1 -IdfPath "D:\esp\esp-idf"
#
#  WHY THIS SCRIPT EXISTS
#  1) 环境：ESP-IDF 的 export.ps1 必须先激活，否则 idf.py / cmake / ninja /
#     xtensa-esp-elf-gcc 都不在 PATH 上。脚本自动定位 IDF 并激活它。
#  2) 中文路径：参考工程当初放在含中文的目录里，ESP-IDF v5.4 在 Windows 上会在
#     三个地方连环出错（kconfig 的 config.env 被按 GBK 解码、ccache 的
#     std::filesystem 转换、ldgen 调 objdump）。本工程路径是纯 ASCII，会原地构建；
#     若你以后把它挪到中文目录，本脚本会【自动把源码镜像到纯 ASCII 目录再构建】，
#     并把 sdkconfig 回写，功能完全不受影响。
#     （用 subst 挂 ASCII 盘符是没用的 —— CMake 会把盘符解析回真实中文路径。）
#  3) 报错友好：构建失败时按已知的报错特征给出针对性的下一步提示。
[CmdletBinding()]
param(
    # ---- 动作开关（可以同时给多个，按合理的顺序依次执行）----
    [switch]$Build,
    [switch]$Flash,
    [switch]$Monitor,
    [switch]$Menuconfig,
    [switch]$SetTarget,
    [switch]$Clean,
    [switch]$FullClean,
    [switch]$Erase,
    [switch]$Size,
    [switch]$Reconfigure,

    # 只打印将要执行的 idf.py 命令，不真正运行（用来检查环境与参数）
    [switch]$DryRun,

    # 串口，例如 COM3 / COM12。留空则自动探测（只有一个串口时自动使用）
    [string]$Port = '',

    # 目标芯片（只有 -SetTarget 时使用）
    [string]$Target = 'esp32s3',

    # 项目路径含非 ASCII 字符时，用来镜像构建的纯 ASCII 目录
    [string]$Mirror = 'C:\smart_home_s3',

    # 显式指定 ESP-IDF 检出目录；留空则自动探测
    [string]$IdfPath = '',

    # 显式指定 IDF tools 目录（含 python_env/ 与 tools/）；留空则自动探测
    [string]$ToolsPath = '',

    # 追加到 idf.py 的原始参数
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = 'Continue'

function Write-Step($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Write-Ok($m)   { Write-Host "  [OK]   $m" -ForegroundColor Green }
function Write-Warn($m) { Write-Host "  [WARN] $m" -ForegroundColor Yellow }
function Write-Err($m)  { Write-Host "  [FAIL] $m" -ForegroundColor Red }
function Write-Tip($m)  { Write-Host "         $m" -ForegroundColor Magenta }

function Test-Ascii([string]$s) {
    foreach ($ch in $s.ToCharArray()) { if ([int]$ch -gt 127) { return $false } }
    return $true
}

# 0. project root = two levels above this script's folder (tools/build/)
$projectDir = (Get-Item (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))).FullName
Write-Step 'Source project'
Write-Host "         $projectDir"

# 1. decide where we actually build
#    (纯 ASCII 路径 -> 原地构建；否则镜像到 $Mirror 再构建)
$buildRoot   = $projectDir
$usingMirror = $false

if (-not (Test-Ascii $projectDir)) {
    Write-Warn 'Project path contains NON-ASCII characters.'
    Write-Warn 'ESP-IDF cannot build in place here (kconfig / ccache / ldgen all break).'
    Write-Warn 'Mirroring sources to the ASCII path below and building THERE:'
    Write-Host  "         $Mirror" -ForegroundColor Yellow

    if (-not (Test-Ascii $Mirror)) {
        Write-Err "Mirror path is not ASCII either: $Mirror"
        exit 1
    }
    if (-not (Test-Path $Mirror)) {
        New-Item -ItemType Directory -Path $Mirror -Force | Out-Null
    }

    Write-Step '1) robocopy source -> mirror (excluding build/, logs)'
    robocopy $projectDir $Mirror /E /XD build /XF build_log.txt /NFL /NDL /NJH /NJS /NP | Out-Null
    $rc = $LASTEXITCODE
    if ($rc -ge 8) {
        Write-Err "robocopy failed (exit $rc)"
        exit 1
    }
    Write-Ok "sources synced (robocopy exit $rc)"

    # 把 menuconfig 的设置带进镜像
    if (Test-Path (Join-Path $projectDir 'sdkconfig')) {
        Copy-Item (Join-Path $projectDir 'sdkconfig') (Join-Path $Mirror 'sdkconfig') -Force
        Write-Ok 'sdkconfig copied into the mirror'
    }

    $buildRoot   = $Mirror
    $usingMirror = $true
} else {
    Write-Ok 'Project path is pure ASCII - building in place.'
}

# 2. locate ESP-IDF
if (-not $IdfPath) {
    $cand = @()
    if ($env:IDF_PATH) { $cand += $env:IDF_PATH }
    $cand += @(
        'E:\Espressif\frameworks\esp-idf-v5.4.4'
        'C:\Espressif\frameworks\esp-idf-v5.4.4'
        "$env:USERPROFILE\esp\esp-idf"
        'C:\esp\esp-idf'
        'D:\esp\esp-idf'
    )
    foreach ($base in @('E:\Espressif\frameworks', 'C:\Espressif\frameworks', 'D:\Espressif\frameworks')) {
        if (Test-Path $base) {
            Get-ChildItem $base -Directory -Filter 'esp-idf-*' -ErrorAction SilentlyContinue |
                ForEach-Object { $cand += $_.FullName }
        }
    }
    foreach ($c in $cand) {
        if ($c -and (Test-Path (Join-Path $c 'tools\idf.py'))) { $IdfPath = $c; break }
    }
}

if (-not $IdfPath -or -not (Test-Path (Join-Path $IdfPath 'tools\idf.py'))) {
    Write-Err 'ESP-IDF not found.'
    Write-Tip '请用 Espressif 安装器生成的 "ESP-IDF PowerShell" 快捷方式打开终端，'
    Write-Tip '或者手动指定：-IdfPath "E:\Espressif\frameworks\esp-idf-v5.4.4"'
    exit 1
}
Write-Ok "IDF_PATH = $IdfPath"

# 3. locate tools dir and activate the toolchain
if (-not $ToolsPath) {
    if ($env:IDF_TOOLS_PATH -and (Test-Path (Join-Path $env:IDF_TOOLS_PATH 'python_env'))) {
        $ToolsPath = $env:IDF_TOOLS_PATH
    } else {
        $guess = Split-Path -Parent (Split-Path -Parent $IdfPath)
        if (Test-Path (Join-Path $guess 'python_env')) { $ToolsPath = $guess }
        elseif (Test-Path (Join-Path $guess 'tools'))   { $ToolsPath = $guess }
    }
}
if ($ToolsPath) {
    $env:IDF_TOOLS_PATH = $ToolsPath
    Write-Ok "IDF_TOOLS_PATH = $ToolsPath"
}

# 把 IDF 自带的 python 虚拟环境放到 PATH 最前面（避免误用系统 Python）
$pyEnv = if ($ToolsPath) { Join-Path $ToolsPath 'python_env' } else { $null }
if ($pyEnv -and (Test-Path $pyEnv)) {
    $venv = Get-ChildItem $pyEnv -Directory -Filter 'idf*_py*_env' -ErrorAction SilentlyContinue |
            Select-Object -First 1
    if ($venv) {
        $scripts = Join-Path $venv.FullName 'Scripts'
        if (Test-Path $scripts) { $env:PATH = "$scripts;$env:PATH" }
    }
}

# 让 Python 始终按 UTF-8 处理中文日志，避免乱码
$env:PYTHONUTF8       = '1'
$env:PYTHONIOENCODING = 'utf-8'

if (Test-Path (Join-Path $IdfPath 'export.ps1')) {
    . (Join-Path $IdfPath 'export.ps1') 2>$null
}

# export.ps1 偶尔没把工具链塞进 PATH —— 手动兜底
if (-not (Get-Command xtensa-esp-elf-gcc -ErrorAction SilentlyContinue) -and $ToolsPath) {
    Write-Warn 'export.ps1 did not populate PATH; adding toolchain dirs manually'
    foreach ($pat in @((Join-Path $ToolsPath 'tools\xtensa-esp-elf'),
                       (Join-Path $ToolsPath 'tools\riscv32-esp-elf'))) {
        Get-ChildItem $pat -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            foreach ($sub in @('xtensa-esp-elf\bin', 'riscv32-esp-elf\bin')) {
                $b = Join-Path $_.FullName $sub
                if (Test-Path $b) { $env:PATH = "$b;$env:PATH" }
            }
        }
    }
    foreach ($t in @('cmake', 'ninja', 'ccache')) {
        Get-ChildItem (Join-Path $ToolsPath "tools\$t") -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $b = if ($t -eq 'ninja') { $_.FullName } else { Join-Path $_.FullName 'bin' }
            if (Test-Path $b) { $env:PATH = "$b;$env:PATH" }
        }
    }
}

Write-Step 'Toolchain check'
foreach ($c in 'idf.py', 'cmake', 'ninja', 'xtensa-esp-elf-gcc') {
    $g = Get-Command $c -ErrorAction SilentlyContinue
    if ($g) { Write-Ok "$c -> $($g.Source)" } else { Write-Warn "$c not found" }
}

# 4. pick the serial port (only needed by flash / monitor)
if (-not $Port -and ($Flash -or $Monitor)) {
    try {
        $ports = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
    } catch {
        $ports = @()
    }
    if ($ports.Count -eq 1) {
        $Port = $ports[0]
        Write-Ok "auto-detected serial port: $Port"
    } elseif ($ports.Count -gt 1) {
        Write-Warn "multiple serial ports found: $($ports -join ', ')"
        Write-Tip  '请用 -Port COMx 指定（用设备管理器确认是哪个）'
    } else {
        Write-Warn 'no serial port detected - 开发板插好了吗？USB 线是数据线吗？'
        Write-Tip  '也可以显式指定：-Port COM3'
    }
}

# 5. assemble the idf.py command list
# 没给任何动作开关时，默认只编译
if (-not ($Build -or $Flash -or $Monitor -or $Menuconfig -or $SetTarget -or
          $Clean -or $FullClean -or $Erase -or $Size -or $Reconfigure)) {
    $Build = $true
}

# 会重新生成构建目录 / 需要交互的动作，单独一条 idf.py 调用
$solo = @()
if ($SetTarget)   { $solo += 'set-target' }
if ($Menuconfig)  { $solo += 'menuconfig' }
if ($FullClean)   { $solo += 'fullclean' }

# 其余动作可以串在一条 idf.py 里按顺序执行
$chain = @()
if ($Reconfigure) { $chain += 'reconfigure' }
if ($Clean)       { $chain += 'clean' }
if ($Erase)       { $chain += 'erase-flash' }
if ($Build)       { $chain += 'build' }
if ($Size)        { $chain += 'size' }
if ($Flash)       { $chain += 'flash' }
if ($Monitor)     { $chain += 'monitor' }

$invocations = @()
foreach ($t in $solo) {
    $a = @()
    if ($Port -and $t -ne 'set-target' -and $t -ne 'menuconfig') { $a += @('-p', $Port) }
    if ($t -eq 'set-target') { $a += @($t, $Target) } else { $a += $t }
    $invocations += ,$a
}
if ($chain.Count -gt 0) {
    $a = @()
    if ($Port) { $a += @('-p', $Port) }
    $a += $chain
    if ($ExtraArgs.Count -gt 0) { $a += $ExtraArgs }
    $invocations += ,$a
}

if ($usingMirror -or -not (Test-Ascii $buildRoot)) { $noCcache = $true } else { $noCcache = $false }

$logFile = Join-Path $buildRoot 'build_log.txt'

Write-Step 'Plan'
Write-Host "         build root : $buildRoot"
Write-Host "         tasks      : $(($solo + $chain) -join ', ')"
if ($Port) { Write-Host "         port       : $Port" }
foreach ($inv in $invocations) {
    Write-Host "         > idf.py $($inv -join ' ')" -ForegroundColor Gray
}

if ($DryRun) {
    Write-Step 'Dry run - nothing executed'
    if ($usingMirror) {
        Write-Host '         (mirror refresh is skipped in DryRun)' -ForegroundColor Gray
    }
    exit 0
}

# 6. run idf.py in the build root
Set-Location $buildRoot
$lastExit = 0
$allOutput = New-Object System.Collections.Generic.List[string]

foreach ($inv in $invocations) {
    $idfArgs = @()
    if ($noCcache) { $idfArgs += '--no-ccache' }
    $idfArgs += $inv

    Write-Step "idf.py $($idfArgs -join ' ')   (in $buildRoot)"
    & idf.py @idfArgs 2>&1 | ForEach-Object {
        $line = $_.ToString()
        $allOutput.Add($line) | Out-Null
        Write-Host $line
    }
    $lastExit = $LASTEXITCODE
    if ($lastExit -ne 0) { break }
}

# 留一份日志方便排错（已在 .gitignore 范围外，别提交）
try { $allOutput | Set-Content -Path $logFile -Encoding UTF8 } catch { }

# 7. copy sdkconfig back so menuconfig changes survive
if ($usingMirror) {
    $mirrorCfg = Join-Path $Mirror 'sdkconfig'
    if (Test-Path $mirrorCfg) {
        Copy-Item $mirrorCfg (Join-Path $projectDir 'sdkconfig') -Force
        Write-Ok 'sdkconfig copied back to the source project'
    }
}

# 8. friendly diagnosis on failure
if ($lastExit -ne 0) {
    $text = ($allOutput -join "`n")
    Write-Host ''
    Write-Err "idf.py failed (exit $lastExit)"
    Write-Host "         full log: $logFile" -ForegroundColor Gray
    Write-Step '可能的原因与处理'

    $hit = $false

    if ($text -match 'kconfigs\.in' -or $text -match 'FileNotFoundError.*config\.env') {
        $hit = $true
        Write-Tip '① kconfig 打不开 build/config.env —— 工程路径含非 ASCII 字符。'
        Write-Tip '   处理：让脚本自己镜像（把工程挪回英文路径，或指定 -Mirror "C:\smart_home_s3"）。'
    }
    if ($text -match 'Cannot convert character sequence') {
        $hit = $true
        Write-Tip '② ccache 无法转换路径 —— 同样是中文路径。'
        Write-Tip '   处理：脚本在非 ASCII 路径下会自动加 --no-ccache；或直接用纯英文路径。'
    }
    if ($text -match 'libxtensa\.a' -or $text -match 'objdump.*No such file') {
        $hit = $true
        Write-Tip '③ ldgen 调用 objdump 失败 —— 中文路径的第三个坑。'
    }
    if ($text -match 'Python interpreter is not supported' -or $text -match 'python.*not supported') {
        $hit = $true
        Write-Tip '④ 用错 Python 了。请用 Espressif 安装器生成的 "ESP-IDF PowerShell" 快捷方式，'
        Write-Tip '   或先执行 IDF 的 export.ps1（本脚本已尝试自动做这件事）。'
    }
    if ($text -match 'unknown target' -or $text -match 'Invalid target') {
        $hit = $true
        Write-Tip '⑤ 目标芯片没设对。先跑：pwsh -File tools\build\build.ps1 -SetTarget'
    }
    if ($text -match 'CMake Error.*sdkconfig' -or $text -match 'sdkconfig.*invalid') {
        $hit = $true
        Write-Tip '⑥ sdkconfig 与 sdkconfig.defaults 冲突（改过 defaults 但没重新生成 sdkconfig）。'
        Write-Tip '   处理：pwsh -File tools\build\build.ps1 -Clean，再 -SetTarget，再 -Build。'
    }
    if ($text -match 'Failed to connect to ESP32' -or $text -match 'No serial data received' -or
        $text -match 'could not open port' -or $text -match 'The port is busy') {
        $hit = $true
        Write-Tip '⑦ 串口/烧录问题：'
        Write-Tip '   · 串口被别的软件占用（关掉串口助手 / 另一个 monitor）'
        Write-Tip '   · -Port 指定错了（设备管理器里确认 COM 号）'
        Write-Tip '   · 板子没进下载模式：按住 BOOT → 点一下 RST → 松开 BOOT，再重试'
        Write-Tip '   · USB 线只有电源线没有数据线'
    }
    if ($text -match "not a valid component" -or $text -match 'Failed to resolve component') {
        $hit = $true
        Write-Tip '⑧ 组件问题：components/ 下的每个组件目录都必须有 CMakeLists.txt 且调用 idf_component_register()；'
        Write-Tip '   往 BSP 里加新驱动（如 TFT/BUZZER）时，记得同步登记到 components/BSP/CMakeLists.txt 的 SRCS。'
    }
    if ($text -match 'region .* overflowed' -or $text -match 'does not fit') {
        $hit = $true
        Write-Tip '⑨ 固件超出分区大小：检查 partitions-16MiB.csv，或降低 CONFIG_COMPILER_OPTIMIZATION 等级。'
    }
    if (-not $hit) {
        Write-Tip '未匹配到已知特征。请把上面最后 30 行日志贴出来，或看 build_log.txt。'
        Write-Tip '常见做法：pwsh -File tools\build\build.ps1 -FullClean -Build'
    }
} else {
    Write-Host ''
    Write-Host '=== idf.py finished OK ===' -ForegroundColor Green
    if ($Build) {
        $sizeLine = $allOutput | Where-Object { $_ -match 'binary size' } | Select-Object -Last 2
        foreach ($l in $sizeLine) { Write-Host "    $($l.Trim())" -ForegroundColor Green }
    }
    if ($usingMirror) {
        Write-Host "    build artifacts: $Mirror\build" -ForegroundColor Green
    }
    if ($Flash -or $Monitor) {
        Write-Host '    提示：monitor 里按 Ctrl+] 退出' -ForegroundColor Green
    }
}

exit $lastExit
