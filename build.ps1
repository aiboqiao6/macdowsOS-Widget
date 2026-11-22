<#
.SYNOPSIS
    构建 macdowsOS Widget，产出可双击运行的 exe。

.DESCRIPTION
    - 自动定位 MSBuild 与 Qt，不需要先打开 Visual Studio。
    - 编译前检查程序是否正在运行：exe 被占用时 Windows 不允许覆盖，会报 LNK1104，
      所以这里直接拦下并提示先退出。
    - Qt 路径优先取工程目录里的 "macdowsOS Widget.vcxproj.user"；该文件不存在时
      用 -QtDir 或下面的默认路径传给 MSBuild。
    - Qt 的 DLL 与插件目录由工程的后构建自动部署到 exe 旁边，不需要手动 windeployqt。

.EXAMPLE
    .\build.ps1
    编 Release 版。

.EXAMPLE
    .\build.ps1 Debug
    编 Debug 版。

.EXAMPLE
    .\build.ps1 Release -Clean
    全量重编（改了 qrc 资源、主题 JSON、qss 之类时用）。

.EXAMPLE
    .\build.ps1 Release -Run
    编完直接启动。
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Clean,
    [switch]$Run,
    [string]$QtDir = 'E:\Apps\QT\6.12.0\msvc2022_64'
)

$ErrorActionPreference = 'Stop'
$projectDir = $PSScriptRoot
$project = Join-Path $projectDir 'macdowsOS Widget.vcxproj'
$userFile = "$project.user"
$exeDir = Join-Path $projectDir "x64\$Configuration"
$exe = Join-Path $exeDir 'macdowsOSWidget.exe'

function Write-Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }
function Write-Fail($text) { Write-Host "!!  $text" -ForegroundColor Red }

# ---- 0. 工程与运行状态 ------------------------------------------------------
if (-not (Test-Path $project)) {
    Write-Fail "找不到工程文件：$project"
    exit 1
}

$running = Get-Process -Name 'macdowsOSWidget' -ErrorAction SilentlyContinue
if ($running) {
    # 只有正在运行同一配置的 exe 才会锁住链接，另一个配置可以照常编译。
    $locked = @($running | Where-Object {
            try { $_.Path -and ([string]::Equals($_.Path, $exe, 'InvariantCultureIgnoreCase')) } catch { $false }
        })
    if ($locked.Count -gt 0) {
        Write-Fail "程序正在运行（PID $($locked.Id -join ', ')），$Configuration 的 exe 被占用，编译会失败。"
        Write-Host "    请先在托盘图标上右键 → 退出，然后重新运行本脚本。" -ForegroundColor Yellow
        exit 2
    }
    Write-Host "    注意：程序正在运行（PID $($running.Id -join ', ')），编译的是另一个配置，可以继续。" -ForegroundColor Yellow
}

# ---- 1. 定位 MSBuild --------------------------------------------------------
Write-Step "定位 MSBuild"
$msbuild = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $found = & $vswhere -latest -products * -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1
    if ($found) { $msbuild = $found }
}
if (-not $msbuild) {
    foreach ($candidate in @(
            (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'),
            (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe'),
            (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe'))) {
        if (Test-Path $candidate) { $msbuild = $candidate; break }
    }
}
if (-not $msbuild) {
    Write-Fail "找不到 MSBuild.exe，请确认已安装 Visual Studio 2022（含 C++ 桌面开发工作负载）。"
    exit 1
}
Write-Host "    $msbuild"

# ---- 2. 组装参数 ------------------------------------------------------------
Write-Step "准备 $Configuration|x64$(if ($Clean) { '（全量重编）' })"
$buildArgs = @(
    $project
    "/p:Configuration=$Configuration"
    '/p:Platform=x64'
    '/m'
    '/nologo'
    '/v:minimal'
)
if ($Clean) { $buildArgs += '/t:Rebuild' }

# 工程里的 QtInstall 写的是版本名（如 6.11.1_msvc2022_64），这台机器上没有注册过。
# .vcxproj.user 会用真实路径覆盖它；没有该文件时在这里补上。
if (Test-Path $userFile) {
    Write-Host "    Qt 路径取自 $([System.IO.Path]::GetFileName($userFile))"
} elseif ($QtDir -and (Test-Path (Join-Path $QtDir 'bin\qmake.exe'))) {
    Write-Host "    Qt 路径由 -QtDir 传入：$QtDir"
    $buildArgs += "/p:QtInstall=$QtDir"
} else {
    Write-Fail "既没有 $([System.IO.Path]::GetFileName($userFile))，-QtDir 也不指向有效的 Qt 安装：$QtDir"
    exit 1
}

# ---- 3. 编译 ----------------------------------------------------------------
Write-Step "开始编译"
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$output = & $msbuild $buildArgs 2>&1
$code = $LASTEXITCODE
$clock.Stop()
$output | Set-Content (Join-Path $env:TEMP 'macdowsOS-build.log') -Encoding UTF8

$problems = $output | Select-String -Pattern 'error C\d|error LNK|error MSB|fatal error'
if ($code -ne 0 -or $problems) {
    Write-Fail "编译失败（exit=$code，用时 $([int]$clock.Elapsed.TotalSeconds) 秒）"
    $problems | Select-Object -First 20 | ForEach-Object { Write-Host "    $($_.Line)" -ForegroundColor Red }
    $hint = $problems | Select-String -Pattern 'LNK1104' | Select-Object -First 1
    if ($hint) {
        Write-Host "    提示：LNK1104 说明目标 exe 还在被占用，检查是否还有进程没退出。" -ForegroundColor Yellow
    }
    Write-Host "    完整日志：$(Join-Path $env:TEMP 'macdowsOS-build.log')" -ForegroundColor Yellow
    exit 1
}

# ---- 4. 结果 ----------------------------------------------------------------
if (-not (Test-Path $exe)) {
    Write-Fail "编译返回成功，但没有找到产物：$exe"
    exit 1
}
$info = Get-Item $exe
Write-Step "完成，用时 $([int]$clock.Elapsed.TotalSeconds) 秒"
Write-Host ("    {0}" -f $info.FullName)
Write-Host ("    {0:yyyy-MM-dd HH:mm:ss}  {1:N2} MB" -f $info.LastWriteTime, ($info.Length / 1MB))
$qtDlls = (Get-ChildItem $exeDir -Filter 'Qt6*.dll' -ErrorAction SilentlyContinue | Measure-Object).Count
Write-Host "    旁边已有 $qtDlls 个 Qt DLL（由工程后构建自动部署）"

if ($Run) {
    Write-Step "启动"
    Start-Process -FilePath $info.FullName
}
exit 0
