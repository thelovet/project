# switch_mode.ps1 - switch between BURN mode and NORMAL GAME mode
# Usage:
#   powershell -File switch_mode.ps1 burn   -> VIDEO_CHUNK_STORE_MODE ON
#   powershell -File switch_mode.ps1 game   -> VIDEO_CHUNK_STORE_MODE OFF
#   powershell -File switch_mode.ps1        -> show current state
# NOTE: both modes KEEP -flto (GCC12 实测: 字库/按钮/视频/游戏 四种模式都能链过, LTO 省约 4KB)
param([string]$mode, [string]$ProjPath = '')

$latin = [System.Text.Encoding]::GetEncoding(28591)   # byte<->char 1:1, keeps GBK/UTF-8 intact

# --- 路径: 脚本可以放在任意位置(现在: <工程>\BIN文件烧录器\), 所以从脚本目录向上找工程根 ---
function Find-Proj([string]$startDir) {
    $d = $startDir
    for ($i = 0; $i -lt 8; $i++) {
        if (Test-Path (Join-Path $d 'SPI\SPI_LCD\User\src')) { return $d }
        $p = Split-Path $d -Parent
        if (-not $p -or $p -eq $d) { break }
        $d = $p
    }
    return ''
}

# 脚本在工程外时(原位置 ...\文件数据与源代码\数据\娜娜莉\): 去 ../../源代码/ 下逐个子目录找
function Find-Proj-Sibling([string]$startDir) {
    foreach ($up in @('..\..', '..\..\..')) {
        $srcRoot = Join-Path (Join-Path $startDir $up) '源代码'
        if (-not (Test-Path $srcRoot)) { continue }
        foreach ($d in (Get-ChildItem -Path $srcRoot -Directory -ErrorAction SilentlyContinue)) {
            if (Test-Path (Join-Path $d.FullName 'SPI\SPI_LCD\User\src')) { return $d.FullName }
        }
    }
    return ''
}
if ($ProjPath -ne '') {
    $proj = (Get-Item -LiteralPath $ProjPath).FullName
} else {
    $proj = Find-Proj $PSScriptRoot
    if ($proj -eq '') { $proj = Find-Proj-Sibling $PSScriptRoot }
}
if ($proj -eq '' -or -not (Test-Path $proj)) {
    Write-Host "找不到工程根(应含 SPI\SPI_LCD\User\src): 请用 -ProjPath <工程目录> 指定"
    exit 1
}
$cfg  = Join-Path $proj "SPI\SPI_LCD\User\inc\config.h"
$obj  = Join-Path $proj "SPI\SPI_LCD\obj"

function Read-Text($path) { return $latin.GetString([IO.File]::ReadAllBytes($path)) }
function Write-Text($path, $s) { [IO.File]::WriteAllBytes($path, $latin.GetBytes($s)) }

# --- config.h: define/comment VIDEO_CHUNK_STORE_MODE ---
function Set-BurnMacro([bool]$on) {
    $s = Read-Text $cfg
    if ($on) {
        $s = [regex]::Replace($s, '(?m)^//#define VIDEO_CHUNK_STORE_MODE', '#define VIDEO_CHUNK_STORE_MODE')
    } else {
        $s = [regex]::Replace($s, '(?m)^#define VIDEO_CHUNK_STORE_MODE', '//#define VIDEO_CHUNK_STORE_MODE')
    }
    Write-Text $cfg $s
}

# --- add/remove -flto in every compile/link command line ---
# anchor: after "-fno-common" (present in all gcc command lines); fallback after "-Os "
# $wantBurn = $true  -> remove -flto (BURN mode); $false -> add -flto (GAME mode)
function Set-FltoFile($path, [bool]$wantBurn) {
    $s = Read-Text $path
    $eol = if ($s.Contains("`r`n")) { "`r`n" } else { "`n" }
    $lines = $s -split "`r?`n"
    $changed = $false
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $L = $lines[$i]
        if ($L -match 'riscv-wch-elf-gcc' -and ($L -match '-c -o ' -or $L -match 'SPI_LCD\.elf')) {
            $n = $L
            if ($wantBurn) {
                $n = [regex]::Replace($n, '\s+-flto', '')
            } else {
                $n = [regex]::Replace($n, '\s+-flto', '')           # dedupe first
                if ($n -notmatch '-flto') {
                    $n = [regex]::Replace($n, '(-fno-common)', '$1 -flto')
                    if ($n -notmatch '-flto') { $n = [regex]::Replace($n, '(-Os )', '$1-flto ') }
                }
            }
            if ($n -ne $L) { $lines[$i] = $n; $changed = $true }
        }
    }
    if ($changed) { Write-Text $path ($lines -join $eol) }
    return $changed
}

function Get-FltoCount {
    $n = 0
    $files = @(Get-ChildItem -Path $obj -Recurse -Filter *.mk) + @(Get-Item "$obj\makefile")
    foreach ($f in $files) {
        $s = Read-Text $f.FullName
        if ($s -match '\s+-flto') { $n++ }
    }
    return $n
}

# --- current state ---
function Get-BurnState {
    $s = Read-Text $cfg
    return ($s -match '(?m)^#define VIDEO_CHUNK_STORE_MODE')
}

if (-not $mode) {
    $burn = Get-BurnState
    if ($burn) { Write-Output "current mode: BURN (VIDEO_CHUNK_STORE_MODE defined)" }
    else       { Write-Output "current mode: GAME (VIDEO_CHUNK_STORE_MODE commented)" }
    Write-Output ("files still with -flto: {0}" -f (Get-FltoCount))
    Write-Output "usage: powershell -File switch_mode.ps1 burn|game"
    exit 0
}

$wantBurn = $false
if ($mode -eq 'burn') { $wantBurn = $true }
elseif ($mode -eq 'game') { $wantBurn = $false }
else { Write-Output ("unknown mode: {0}" -f $mode); exit 1 }

# 1. config.h macro
Set-BurnMacro $wantBurn

# 2. -flto: keep as-is, do NOT strip it.
#    (GCC12 + LTO 已验证: 字库/按钮/视频/游戏 四种模式都能链过且体积更小)
#    (原"删除 -flto"的循环已移除; Set-FltoFile / Get-FltoCount 保留备用/自查)
$nChanged = 0

# 3. clean to force rebuild with new options
$gcc  = "D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC12\bin"
$mkbn = "D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\others\Build_Tools\Make\bin"
$env:Path = "$gcc;$mkbn;" + $env:Path
Push-Location $obj
& make clean *> $null
Pop-Location

# 4. report
$burn = Get-BurnState
if ($burn) {
    Write-Output ("switched to: BURN mode (VIDEO_CHUNK_STORE_MODE defined, LTO kept), {0} files updated, make clean done" -f $nChanged)
    Write-Output "next: python gen_video_chunk.py <N> then run burn_all.ps1 (it does gen + make itself)"
} else {
    Write-Output ("switched to: GAME mode (VIDEO_CHUNK_STORE_MODE commented, LTO kept), {0} files updated, make clean done" -f $nChanged)
    Write-Output "next: make all, then SWD download (openocd program ... reset exit)"
}
