# 读真机帧耗时采样(标定模拟器用)
#
# 原理: 固件在 decompress.c 里把每帧"纯处理耗时"(us, 不含节流等待)记进 RAM 数组
#       g_perf_log[], 容量 64 帧, 录满即停。
#       本脚本用 WCH-Link 让 CPU 暂停一下(halt), 把这几个变量从 RAM 里读出来, 再 resume。
#       不显示在屏上、不写 flash, 所以不会干扰计时。
#
# 用法: 烧好固件 -> 播一会儿视频(让它录满) -> 运行本脚本。
#       0°/180° 各跑一次, 各记一组数, 用来标定模拟器的 CPU 常数。

$ErrorActionPreference = 'Continue'
$openocd = 'D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD\bin\openocd.exe'
$cfg     = 'D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD\bin\wch-riscv.cfg'
$nm      = 'D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC12\bin\riscv-wch-elf-nm.exe'
$elf     = Join-Path $PSScriptRoot 'SPI\SPI_LCD\obj\SPI_LCD.elf'
$tmpDir  = Join-Path $env:TEMP 'fps_read'

foreach ($p in @($openocd, $cfg, $nm, $elf)) {
    if (-not (Test-Path -LiteralPath $p)) { Write-Host ("找不到: {0}" -f $p); exit 1 }
}
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

function Get-Sym([string]$name) {
    $line = & $nm $elf 2>$null | Select-String -Pattern ('\s' + [regex]::Escape($name) + '$') | Select-Object -First 1
    if (-not $line) { return 0 }
    return [Convert]::ToUInt32((($line.Line -split '\s+')[0]), 16)
}

$aLog = Get-Sym 'g_perf_log'
$aCnt = Get-Sym 'g_perf_cnt'
$aAvg = Get-Sym 'g_perf_avg_us'
$aLow = Get-Sym 'g_perf_low_fps'
if (-not $aLog -or -not $aCnt) {
    Write-Host '符号没找到 —— 需要重新编译带采样的固件(先跑 编译与下载.ps1 build)'
    exit 1
}
Write-Host ("g_perf_log=0x{0:X8}  g_perf_cnt=0x{1:X8}" -f $aLog, $aCnt)

$fLog = Join-Path $tmpDir 'log.bin'
$fCnt = Join-Path $tmpDir 'cnt.bin'
$fAvg = Join-Path $tmpDir 'avg.bin'
$fLow = Join-Path $tmpDir 'low.bin'

$cmd = @(
    '-f', $cfg,
    '-c', 'adapter speed 4000',
    '-c', 'chip_id CH32V20x',
    '-c', 'init',
    '-c', 'halt',
    '-c', ('dump_image {{{0}}} 0x{1:X8} 256' -f $fLog, $aLog),
    '-c', ('dump_image {{{0}}} 0x{1:X8} 4' -f $fCnt, $aCnt),
    '-c', ('dump_image {{{0}}} 0x{1:X8} 4' -f $fAvg, $aAvg),
    '-c', ('dump_image {{{0}}} 0x{1:X8} 4' -f $fLow, $aLow),
    '-c', 'resume',
    '-c', 'shutdown'
)
$out = ''
for ($try = 1; $try -le 10; $try++) {
    Remove-Item -LiteralPath $fLog -ErrorAction SilentlyContinue
    $out = & $openocd @cmd 2>&1 | Out-String
    if (Test-Path -LiteralPath $fLog) { break }
    Write-Host ("第 {0} 次连接失败 —— 固件待机时 SDI 调试口会关闭。" -f $try)
    Write-Host '  请点一下屏幕让它亮起来(视频播放中), 1.5 秒后重试...'
    Start-Sleep -Milliseconds 1500
}

if (-not (Test-Path -LiteralPath $fLog)) {
    Write-Host $out
    Write-Host '读取失败: 板子没连上(多半还在待机), 或者调试器被占用'
    Write-Host '提示: 先点屏幕唤醒/让它播视频, 趁亮着立刻重跑本脚本。'
    exit 1
}

$cntBytes = [IO.File]::ReadAllBytes($fCnt)
$cnt = [BitConverter]::ToUInt32($cntBytes, 0)
if ($cnt -gt 64) { $cnt = 64 }

$vals = @()
if ($cnt -gt 0) {
    $bytes = [IO.File]::ReadAllBytes($fLog)
    for ($i = 0; $i -lt $cnt; $i++) {
        $v = [BitConverter]::ToUInt32($bytes, $i * 4)
        if ($v -gt 0) { $vals += $v }
    }
}
$avgUs = [BitConverter]::ToUInt32([IO.File]::ReadAllBytes($fAvg), 0)
$lowFps = [BitConverter]::ToUInt32([IO.File]::ReadAllBytes($fLow), 0)
Remove-Item -LiteralPath $fLog, $fCnt, $fAvg, $fLow -ErrorAction SilentlyContinue

Write-Host ''
Write-Host ('采样帧数: {0}' -f $cnt)
if ($vals.Count -eq 0) {
    Write-Host '没有有效采样 —— 先在视频模式播一会儿再读'
    exit 0
}

$sum = 0; $mn = [uint32]::MaxValue; $mx = 0
foreach ($v in $vals) { $sum += $v; if ($v -lt $mn) { $mn = $v }; if ($v -gt $mx) { $mx = $v } }
$avg = $sum / $vals.Count

Write-Host ('平均处理耗时 : {0:N2} ms  ->  {1:N1} fps' -f ($avg / 1000), (1000000.0 / $avg))
Write-Host ('最快(上限)   : {0:N2} ms  ->  {1:N1} fps' -f ($mn / 1000), (1000000.0 / $mn))
Write-Host ('最慢(掉帧)   : {0:N2} ms  ->  {1:N1} fps' -f ($mx / 1000), (1000000.0 / $mx))
Write-Host ('固件自算 avg_frame_time: {0:N2} ms' -f ($avgUs / 1000))
Write-Host ('固件 GetLowestFPS()    : {0} fps' -f $lowFps)
