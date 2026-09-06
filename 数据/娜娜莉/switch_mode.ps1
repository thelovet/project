param([string]$mode = "")

# ================================================================
# switch_mode.ps1 - 切换 烧录(burn)/视频(video)/游戏(game) 模式
# 同步改:
#   - config.h 的模式宏 (FLASH_DATA_STORE_MODE / VIDEO_CHUNK_STORE_MODE)
#   - obj/makefile 与 obj/**/subdir.mk 里的 -flto (MRS 会重建加回, 命令行构建需去掉)
# 用法:
#   powershell -File "数据\娜娜莉\switch_mode.ps1" burn   # 烧录模式
#   powershell -File "数据\娜娜莉\switch_mode.ps1" video  # 视频烧录模式
#   powershell -File "数据\娜娜莉\switch_mode.ps1" game   # 游戏模式
#   powershell -File "数据\娜娜莉\switch_mode.ps1"        # 查看当前模式
# ================================================================

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = (Resolve-Path (Join-Path $scriptDir "..\..")).Path
$configH   = Join-Path $repoRoot "SPI\SPI_LCD\User\inc\config.h"
$objRoot   = Join-Path $repoRoot "SPI\SPI_LCD\obj"

if (-not (Test-Path $configH)) {
    Write-Host "找不到 config.h: $configH" -ForegroundColor Red
    exit 1
}

# ---- 编码安全读写 (严格 UTF-8 失败则退回 GBK/936) ----
function Read-Text {
    param([string]$Path)
    $b = [IO.File]::ReadAllBytes($Path)
    $utf8 = New-Object System.Text.UTF8Encoding($false, $true)
    try { return ,@([string]$utf8.GetString($b), 'utf8') }
    catch { return ,@([Text.Encoding]::GetEncoding(936).GetString($b), 'gbk') }
}
function Write-Text {
    param([string]$Path, [string]$Text, [string]$Enc)
    if ($Enc -eq 'utf8') { $e = New-Object System.Text.UTF8Encoding($false) }
    else { $e = [Text.Encoding]::GetEncoding(936) }
    [IO.File]::WriteAllText($Path, $Text, $e)
}

# ---- 每行的模式行 (含注释文本, 中文; 脚本文件按 GBK 保存) ----
$FLASH_OFF    = '//#define FLASH_DATA_STORE_MODE   /* 烧录完成, 恢复游戏模式 */'
$FLASH_BURN   = '#define FLASH_DATA_STORE_MODE   /* 烧录模式 (烧完需注释回),恢复游戏模式 */'
$VIDEO_OFF    = '//#define VIDEO_CHUNK_STORE_MODE'
$VIDEO_BURN   = '#define VIDEO_CHUNK_STORE_MODE   /* 视频烧录模式 */'

$r = Read-Text $configH
$cfg = $r[0]
$cfgEnc = $r[1]
$lines = $cfg -split "`n"

function Set-Line {
    param($lines, [string]$macro, [string]$val)
    $re = '#define\s+' + [regex]::Escape($macro)
    for ($i=0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match $re) { $lines[$i] = $val }
    }
    return $lines
}

switch ($mode) {
    "burn" {
        $lines = Set-Line $lines 'FLASH_DATA_STORE_MODE' $FLASH_BURN
        $lines = Set-Line $lines 'VIDEO_CHUNK_STORE_MODE' $VIDEO_OFF
        Write-Host "已切换 -> 烧录模式 (FLASH_DATA_STORE_MODE)" -ForegroundColor Cyan
    }
    "video" {
        $lines = Set-Line $lines 'FLASH_DATA_STORE_MODE' $FLASH_OFF
        $lines = Set-Line $lines 'VIDEO_CHUNK_STORE_MODE' $VIDEO_BURN
        Write-Host "已切换 -> 视频烧录模式 (VIDEO_CHUNK_STORE_MODE)" -ForegroundColor Cyan
    }
    "game" {
        $lines = Set-Line $lines 'FLASH_DATA_STORE_MODE' $FLASH_OFF
        $lines = Set-Line $lines 'VIDEO_CHUNK_STORE_MODE' $VIDEO_OFF
        Write-Host "已切换 -> 游戏模式" -ForegroundColor Cyan
    }
    default {
        $cur = "游戏模式"
        if ($cfg -match '^\s*#define\s+FLASH_DATA_STORE_MODE') { $cur = "烧录模式" }
        elseif ($cfg -match '^\s*#define\s+VIDEO_CHUNK_STORE_MODE') { $cur = "视频烧录模式" }
        Write-Host "当前模式: $cur" -ForegroundColor Green
        exit 0
    }
}
$cfgNew = $lines -join "`n"
if ($cfgNew -ne $cfg) { Write-Text $configH $cfgNew $cfgEnc }

# ---- obj/makefile 与 obj/**/subdir.mk 去 -flto (含 flto 才写回) ----
$mkFiles = @(Join-Path $objRoot "makefile")
$mkFiles += @(Get-ChildItem -Path $objRoot -Recurse -Filter *.mk | Where-Object { $_.Name -ne 'makefile' } | ForEach-Object { $_.FullName })
$cleaned = 0
foreach ($mp in $mkFiles) {
    if (-not (Test-Path $mp)) { continue }
    $rr = Read-Text $mp
    $mt = $rr[0]; $me = $rr[1]
    if ($mt -match 'flto') {
        $nt = $mt -replace '-flto\s*', ' '
        if ($nt -ne $mt) { Write-Text $mp $nt $me; $cleaned++ }
    }
}
if ($cleaned -gt 0) { Write-Host "已从 $cleaned 个 makefile 去掉 -flto" -ForegroundColor Yellow }
else { Write-Host "makefile 无需处理 (无 -flto)" -ForegroundColor DarkGray }
