# PowerShell
# 安卓 App 构建入口（不依赖 gradle-wrapper.jar）

# 为什么不用 gradlew.bat：它需要 gradle-wrapper.jar 这个二进制文件，
# 本工程交付时没带过来。所以直接调用本机已缓存的 Gradle 发行版。
# 用法：
#   build.ps1                      -> assembleDebug
#   build.ps1 assembleRelease      -> 出 release 包
#   build.ps1 assembleDebug assembleRelease
#   build.ps1 clean

[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Task = @('assembleDebug')
)

$ErrorActionPreference = 'Continue'
$proj = $PSScriptRoot

# ---------------- 1. 找 Gradle ----------------
# AGP 8.7 要求 Gradle >= 8.9
$gradle = $null
$dists = Join-Path $env:USERPROFILE '.gradle\wrapper\dists'

foreach ($pat in @('gradle-8.9-bin', 'gradle-8.1?-bin')) {
    if ($gradle) { break }
    Get-ChildItem (Join-Path $dists $pat) -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        if ($gradle) { return }
        Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $bat = Join-Path $_.FullName 'bin\gradle.bat'
            if (Test-Path $bat) { $gradle = $bat }
        }
    }
}
if (-not $gradle) {
    $cmd = Get-Command gradle -ErrorAction SilentlyContinue
    if ($cmd) { $gradle = $cmd.Source }
}
if (-not $gradle) {
    Write-Host ''
    Write-Host '[错误] 找不到 Gradle 8.9+' -ForegroundColor Red
    Write-Host '       两种办法任选其一：'
    Write-Host '         1) 补上 wrapper：把参考工程的 jar 复制过来，再用 gradlew.bat'
    Write-Host '         2) 用 Android Studio 直接 Open 本目录 android\ 后点 Run'
    Write-Host ''
    exit 2
}

# ---------------- 2. 找 JDK ----------------
# AGP 8.7 + Kotlin 要 JDK 17 以上，Android Studio 自带的 JBR 最稳
$jbr = 'C:\Program Files\Android\Android Studio\jbr'
if (Test-Path (Join-Path $jbr 'bin\java.exe')) {
    $env:JAVA_HOME = $jbr
} elseif (-not $env:JAVA_HOME) {
    Write-Host '[警告] 没找到 Android Studio 的 JBR，也没设 JAVA_HOME，将用 PATH 里的 java。' -ForegroundColor Yellow
}

# ---------------- 3. 找 Android SDK ----------------
# local.properties 是 Java properties：`:` 和每一个 `\` 都要转义
$localProps = Join-Path $proj 'local.properties'
if (-not (Test-Path $localProps)) {
    $sdk = $env:ANDROID_HOME
    if (-not $sdk) { $sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
    if (Test-Path $sdk) {
        $escaped = $sdk -replace '\\', '\\' -replace ':', '\:'
        [System.IO.File]::WriteAllText($localProps, "sdk.dir=$escaped`n", (New-Object System.Text.UTF8Encoding($false)))
        Write-Host "[信息] 已生成 $localProps"
    } else {
        Write-Host '[警告] 没找到 Android SDK，请手动创建 local.properties 写入 sdk.dir=...' -ForegroundColor Yellow
    }
}

# ---------------- 4. 构建 ----------------
Write-Host ''
Write-Host "[信息] Gradle   : $gradle"
Write-Host "[信息] JAVA_HOME: $env:JAVA_HOME"
Write-Host "[信息] 任务     : $($Task -join ' ')"
Write-Host ''

# 用本工程自己的 .gradle/build，不共用 ESP-IDF 的 build\
Push-Location $proj
try {
    # 不在这里传 -Dfile.encoding：gradle.properties 里已经设了，
    # 而且 PowerShell 会把带 = 的 -D 参数拆坏
    & $gradle --no-daemon @Task
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($rc -ne 0) {
    Write-Host ''
    Write-Host "[失败] 构建返回码 $rc" -ForegroundColor Red
    exit $rc
}

Write-Host ''
Write-Host '[成功] APK 位置：'
Write-Host "       $proj\app\build\outputs\apk\debug\app-debug.apk"
Write-Host "       $proj\app\build\outputs\apk\release\app-release.apk"
exit 0
