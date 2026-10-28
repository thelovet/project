# 烧录助手(桥)的 PC 端 v2: 单会话连烧 + 大块 + 轮询状态
#
# 前提: 设备里烧的是 BURN_BRIDGE_MODE 固件(约 3.8KB, 只做"把 RAM 缓冲区写进外部 SPI Flash"一件事)。
# 优点: 固件固定不变, 字库/按钮/视频想换就换, 不受内部 Flash 64KB 限制。
#
# 用法:
#   .\burn_bridge.ps1 -BinPath 数据.bin                 # 从地址 0 开始, 8KB 一块, 全烧
#   .\burn_bridge.ps1 -BinPath 数据.bin -BaseAddr 0x10000
#   .\burn_bridge.ps1 -BinPath 数据.bin -Blocks 128     # 只烧前 128 块(测速用)
#
# 协议(与 main.c 的 BurnBridge_Run 对应):
#   g_burn_buf    8KB 缓冲区      <- openocd load_image
#   g_burn_addr   目标地址        <- mww
#   g_burn_len    本块长度        <- mww
#   g_burn_cmd    1=写一块        <- mww
#   g_burn_status 0=忙 0xA5=成功  <- 轮询 mem2array
#
# v2 与 v1 的差别(为什么快):
#   v1: 每块起两次 openocd(写一次、读一次), 外加固定 400ms 盲等 -> 约 1.8s/4KB
#   v2: 一个 openocd 会话里连烧 Batch 块; 状态在 tcl 里轮询(25ms 一次), 写不完就继续等,
#       写完立刻进下一块 -> 时间只花在"灌数据 + Flash 擦写"上, 不再有进程启动和盲等
#
#   注意: 轮询里的 halt 只会暂停内核, 正在移位的 SPI 字节由硬件完成, 不会写坏 Flash;
#         真正要避免的是"没写完后直接以为成功", 所以每块先把 status 清 0, 再等它变 0xA5。

param(
    [string]$BinPath  = '',
    [uint32]$BaseAddr = 0,
    [int]$Chunk    = 8192,    # 每块字节数, 不能超过固件里的 BURN_BUF_SIZE(8192)
    [int]$Blocks   = 0,       # 只烧前 N 块(0=全部), 测速用
    [int]$Batch    = 64,      # 每个 openocd 会话连烧几块(越大越快, 但进度更新越粗)
    [int]$SwdSpeed = 8000,
    [switch]$ExportRes,       # 导出: 把设备外部 Flash 里的"字库+目录"整段读回来存成 bin
    [string]$OutFile = ''     # 导出目标(默认 内部\资源备份.bin)
)

$ErrorActionPreference = 'Stop'

$mrs     = 'D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32'
$openocd = Join-Path $mrs 'components\WCH\OpenOCD\OpenOCD\bin\openocd.exe'
$cfg     = Join-Path $mrs 'components\WCH\OpenOCD\OpenOCD\bin\wch-riscv.cfg'
$nm      = Join-Path $mrs 'components\WCH\Toolchain\RISC-V Embedded GCC12\bin\riscv-wch-elf-nm.exe'
$elf     = Join-Path $PSScriptRoot '..\..\SPI\SPI_LCD\obj\SPI_LCD.elf'

foreach ($p in @($openocd, $cfg, $nm)) { if (-not (Test-Path -LiteralPath $p)) { throw "找不到: $p" } }
if (-not $ExportRes -and -not (Test-Path -LiteralPath $BinPath)) { throw "找不到 bin: $BinPath (或用 -ExportRes 导出)" }
$elf = (Resolve-Path $elf).Path
$obj = Split-Path $elf -Parent

# 每块一个独立的临时文件: 命令是整个会话一次性下发的, 共用同一个文件会读到最后一版
$tmpDir = Join-Path $env:TEMP ("burn_bridge_" + $PID)
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$tclPath = Join-Path $obj 'burn_bridge_run.tcl'

function Get-Sym([string]$name) {
    $line = & $nm $elf 2>$null | Select-String -Pattern ('\s' + [regex]::Escape($name) + '$') | Select-Object -First 1
    if (-not $line) { throw "符号没找到: $name (设备固件是不是 BURN_BRIDGE_MODE?)" }
    return [Convert]::ToUInt32((($line.Line -split '\s+')[0]), 16)
}

$aBuf    = Get-Sym 'g_burn_buf'
$aAddr   = Get-Sym 'g_burn_addr'
$aLen    = Get-Sym 'g_burn_len'
$aCmd    = Get-Sym 'g_burn_cmd'
$aStatus = Get-Sym 'g_burn_status'

if ($Chunk -gt 8192) { throw "Chunk=$Chunk 超出固件缓冲区(8192), 请同时把 main.c 的 BURN_BUF_SIZE 改大并重烧桥固件" }

Write-Host ("缓冲区 0x{0:X8}  addr 0x{1:X8}  cmd 0x{2:X8}  status 0x{3:X8}  chunk={4}" -f $aBuf, $aAddr, $aCmd, $aStatus, $Chunk)

$bytes = $null; $len = 0; $allBlocks = 0; $doBlocks = 0
if (-not $ExportRes) {
    $bytes = [IO.File]::ReadAllBytes((Resolve-Path $BinPath).Path)
    $len   = $bytes.Length
    $allBlocks = [int][math]::Ceiling($len / $Chunk)
    $doBlocks  = $allBlocks
    if ($Blocks -gt 0 -and $Blocks -lt $allBlocks) { $doBlocks = $Blocks }
    Write-Host ("文件 {0} : {1:N0} 字节, 共 {2} 块, 本次烧 {3} 块 -> Flash 0x{4:X8}..0x{5:X8}" -f `
                 (Split-Path $BinPath -Leaf), $len, $allBlocks, $doBlocks, $BaseAddr, ($BaseAddr + $doBlocks * $Chunk - 1))
}

# --- 固件侧的 tcl 支撑: 轮询状态, 避免固定盲等 ---
$tcl = @"
set BS_BUF  $('0x' + $aBuf.ToString('X8'))
set BS_ADDR $('0x' + $aAddr.ToString('X8'))
set BS_LEN  $('0x' + $aLen.ToString('X8'))
set BS_CMD  $('0x' + $aCmd.ToString('X8'))
set BS_STAT $('0x' + $aStatus.ToString('X8'))

proc bs_wait {} {
    global BS_STAT
    set i 0
    while {`$i < 80} {
        sleep 25
        halt
        mem2array s 32 `$BS_STAT 1
        set v `$s(0)
        if {`$v == 0xA5} { resume; return 1 }
        if {`$v != 0}    { resume; return 0 }
        resume
        incr i
    }
    return 0
}

proc bs_block {f addr len idx} {
    global BS_BUF BS_ADDR BS_LEN BS_CMD BS_STAT
    halt
    load_image `$f `$BS_BUF bin
    mww `$BS_ADDR `$addr
    mww `$BS_LEN  `$len
    mww `$BS_STAT 0
    mww `$BS_CMD  1
    resume
    set ok [bs_wait]
    echo "BLK `$idx END `$ok"
}

proc bs_read {addr len idx path} {
    global BS_BUF BS_ADDR BS_LEN BS_CMD BS_STAT
    halt
    mww `$BS_ADDR `$addr
    mww `$BS_LEN  `$len
    mww `$BS_STAT 0
    mww `$BS_CMD  3
    resume
    set ok [bs_wait]
    if {`$ok == 0} { echo "RD `$idx FAIL"; return 0 }
    halt
    dump_image `$path `$BS_BUF `$len
    resume
    echo "RD `$idx OK"
}
"@
[IO.File]::WriteAllText($tclPath, $tcl, (New-Object Text.UTF8Encoding $false))

function Invoke-Ocd([string[]]$cmds) {
    # openocd 把日志写到 stderr; PS 会把 stderr 当错误记录, 用 Stop 会直接中断 -> 这里临时放宽
    $ocdArgs = @('-f', $cfg, '-f', $tclPath) + ($cmds | ForEach-Object { @('-c', $_) })
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        return (& $openocd @ocdArgs 2>&1 | Out-String)
    } finally {
        $ErrorActionPreference = $old
    }
}

# ============ 导出模式: 把设备外部 Flash 里的"资源数据 + 目录"整段读回来 ============
if ($ExportRes) {
    if ($OutFile -eq '') { $OutFile = Join-Path $PSScriptRoot '资源备份.bin' }

    # 读外部 Flash 一段: MCU 读进 g_burn_buf -> openocd dump_image 取回
    # 每 32 块开一个会话(命令全塞一个会话会把命令行撑爆: "filename or extension is too long")
    function Read-Flash([uint32]$fa, [int]$fl, [string]$out) {
        $ms = New-Object System.IO.MemoryStream
        $ofs = 0; $k = 0
        while ($ofs -lt $fl) {
            $parts = New-Object System.Collections.Generic.List[string]
            $cmds  = New-Object System.Collections.Generic.List[string]
            $cmds.Add('adapter speed ' + $SwdSpeed); $cmds.Add('chip_id CH32V20x'); $cmds.Add('init')
            $b = 0
            while ($ofs -lt $fl -and $b -lt 32) {
                $n = [Math]::Min($Chunk, $fl - $ofs)
                $f = Join-Path $tmpDir ('r' + $k.ToString('D4') + '.bin')
                $cmds.Add('bs_read 0x' + ($fa + $ofs).ToString('X8') + ' 0x' + $n.ToString('X') + ' ' + $k + ' {' + ($f -replace '\\', '/') + '}')
                $parts.Add($f); $ofs += $n; $k++; $b++
            }
            $cmds.Add('shutdown')
            $o = Invoke-Ocd $cmds
            if (([regex]::Matches($o, 'RD \d+ OK')).Count -ne $parts.Count) { Write-Host $o; throw ("读取失败 @0x{0:X8}" -f ($fa + $ofs)) }
            foreach ($p in $parts) { $bb = [IO.File]::ReadAllBytes($p); $ms.Write($bb, 0, $bb.Length) }
        }
        [IO.File]::WriteAllBytes($out, $ms.ToArray())
        $ms.Dispose()
    }

    $dirSize = 8 + 128 * 12     # 目录头 8 字节 + 128 项 x 12 字节
    $hdr = Join-Path $tmpDir 'dir_hdr.bin'
    $found = $false
    $dataStart = 0
    # 先试 8MB: 小容量芯片是 3 字节地址, 拿 64MB 地址去读会"回绕"到 8MB 内, 误判成 64MB
    foreach ($fs in @([uint64]0x800000, [uint64]0x4000000)) {
        $dirStart = [uint32]($fs - $dirSize)
        Read-Flash $dirStart 16 $hdr
        $h = [IO.File]::ReadAllBytes($hdr)
        if ($h[0] -ne 0xA5 -or $h[1] -ne 0x5A) { continue }
        $ds = ([uint32]$h[4] -shl 24) -bor ([uint32]$h[5] -shl 16) -bor ([uint32]$h[6] -shl 8) -bor [uint32]$h[7]
        $total = [int]($fs - $ds)
        # 合理性: 资源区只有几十 KB, 不在这个量级说明是别的数据碰巧撞上魔数
        if ($ds -ge $fs -or $total -lt 4096 -or $total -gt 2097152) {
            Write-Host ("  跳过 0x{0:X8}: 目录头算出的资源区 {1:N0} 字节不合理" -f $fs, $total)
            continue
        }
        $dataStart = $ds
        Write-Host ("芯片容量 0x{0:X8}, 资源区 0x{1:X8}..0x{2:X8}, 共 {3:N0} 字节" -f $fs, $dataStart, ($fs - 1), $total)
        Read-Flash $dataStart $total $OutFile
        $found = $true
        break
    }
    if (-not $found) { throw "外部 Flash 里没找到字库目录(魔数 A5 5A), 这台设备可能还没烧过字库" }

    Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host ("已导出: {0}" -f $OutFile)
    Write-Host ("重新烧回去: burn_bridge.ps1 -BinPath `"{0}`" -BaseAddr 0x{1:X8}" -f $OutFile, $dataStart)
    exit 0
}

$sw = [Diagnostics.Stopwatch]::StartNew()
$done = 0
$fail = $false

for ($b0 = 0; $b0 -lt $doBlocks; $b0 += $Batch) {
    $b1 = [Math]::Min($b0 + $Batch, $doBlocks)
    $cmds = New-Object System.Collections.Generic.List[string]
    $cmds.Add('adapter speed ' + $SwdSpeed)
    $cmds.Add('chip_id CH32V20x')
    $cmds.Add('init')
    for ($i = $b0; $i -lt $b1; $i++) {
        $off = $BaseAddr + $i * $Chunk
        $n   = [Math]::Min($Chunk, $len - $i * $Chunk)
        $slice = New-Object byte[] $n
        [Array]::Copy($bytes, $i * $Chunk, $slice, 0, $n)
        $f = Join-Path $tmpDir ('c' + $i.ToString('D5') + '.bin')
        [IO.File]::WriteAllBytes($f, $slice)
        $cmds.Add('bs_block {' + ($f -replace '\\', '/') + '} 0x' + $off.ToString('X8') + ' 0x' + $n.ToString('X') + ' ' + $i)
    }
    $cmds.Add('shutdown')

    $out = Invoke-Ocd $cmds
    $okCnt = ([regex]::Matches($out, 'BLK \d+ END 1')).Count
    if ($okCnt -ne ($b1 - $b0)) {
        Write-Host $out
        Write-Host ("本批 {0} 块里只有 {1} 块成功, 已停止" -f ($b1 - $b0), $okCnt)
        $fail = $true
        break
    }
    $done = $b1
    $kb = $done * $Chunk / 1024
    $s  = $sw.Elapsed.TotalSeconds
    Write-Host ("  {0,7:N0} KB / {1,7:N0} KB  ({2,3}%)  {3,7:N1}s  {4,5:N1} KB/s" -f `
                $kb, ($doBlocks * $Chunk / 1024), [int]($done * 100 / $doBlocks), $s, ($kb / $s))
}

if (-not $fail) {
    # 收尾: cmd=9 (固件回 0x5A, 只是握手, 不做别的事)
    $null = Invoke-Ocd @('adapter speed ' + $SwdSpeed, 'chip_id CH32V20x', 'init', 'halt',
                         ('mww 0x{0:X8} 9' -f $aCmd), 'resume', 'shutdown')
}

Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
if ($fail) { throw "烧录中断" }

$tot = $done * $Chunk
Write-Host ("完成: {0:N0} 字节, 用时 {1:N1}s, 平均 {2:N1} KB/s" -f `
            $tot, $sw.Elapsed.TotalSeconds, ($tot / 1024 / $sw.Elapsed.TotalSeconds))
