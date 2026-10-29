# uart_burn.ps1 - 用串口(软 UART 固件)把数据写进外部 Flash
# 用法: powershell -File uart_burn.ps1 [-Com COM10] [-Baud 460800] [-BinPath <视频.bin>] [-StartByte 0] [-Count 0]
# 协议(与 flash_store.c 的 UartStore_BurnLoop 对应):
#   帧 = B5 5A addr(4,LE) len(2,LE) data(len) crc16(2,LE)
#   addr 必须 4KB 对齐, len 1..4096; crc16 = MODBUS(初值 0xFFFF), 只算 data
#   应答: 0x06=成功  0x15=CRC错(重发)  0x16=参数错
param(
    [string]$Com = 'COM10',
    [int]$Baud = 460800,
    [string]$BinPath = '',
    [long]$StartByte = 0,
    [int]$Count = 0,          # 0 = 到文件末尾; 否则只发 N 帧
    [int]$Retry = 3,
    [int]$AckTimeoutMs = 1500,
    [int]$TargetBaud = 0,     # >0 时先用 $Baud 发控制帧切到这个速率, 再开始烧
    [switch]$NoErase,         # 跳过"整片块擦除"(默认会先擦)
    [switch]$Stream,          # 流式模式: 分块连续发(带整块 CRC), MCU 收/写重叠
    [int]$ChunkKB = 1024,     # 流式模式每块大小(KB, 必须是 4 的倍数)
    [switch]$Ping             # 只做链路检测(发 ping 帧), 不烧录
)

$ErrorActionPreference = 'Stop'

if ($BinPath -eq '') {
    $BinPath = Join-Path $PSScriptRoot 'cache\imported_video.bin'
}
$bin = (Get-Item -LiteralPath $BinPath).FullName
$total = (Get-Item -LiteralPath $bin).Length
"源文件: {0}  ({1:N0} 字节)" -f $bin, $total

$FRAME_SIZE = 4096

# ---- CRC16/MODBUS, 与固件完全一致 ----
$crcTab = New-Object 'uint16[]' 256
for ($i = 0; $i -lt 256; $i++) {
    $c = [uint16]$i
    for ($k = 0; $k -lt 8; $k++) {
        if ($c -band 1) { $c = [uint16](($c -shr 1) -bxor 0xA001) } else { $c = [uint16]($c -shr 1) }
    }
    $crcTab[$i] = $c
}
function Crc16([byte[]]$data, [int]$offset, [int]$len) {
    $crc = [uint16]0xFFFF
    for ($i = 0; $i -lt $len; $i++) {
        $crc = [uint16](($crc -shr 8) -bxor $crcTab[($crc -bxor $data[$offset + $i]) -band 0xFF])
    }
    return $crc
}

$sp = New-Object System.IO.Ports.SerialPort($Com, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$sp.ReadTimeout = 200
$sp.WriteTimeout = 3000
$sp.ReadBufferSize = 65536
$sp.WriteBufferSize = 65536
$sp.Open()
"已打开 {0} @ {1}" -f $Com, $Baud
Start-Sleep -Milliseconds 300
$sp.DiscardInBuffer()

# ---- 可选: 先发控制帧切波特率, 再用新速率收发 ----
if ($TargetBaud -gt 0 -and $TargetBaud -ne $Baud) {
    $k = [int]($TargetBaud / 1000)
    $cf = New-Object byte[] 8
    $cf[0] = 0xB5; $cf[1] = 0x5A; $cf[2] = 0xF0; $cf[3] = 0xFF; $cf[4] = 0xFF; $cf[5] = 0xFF
    $cf[6] = [byte]($k -band 0xFF); $cf[7] = [byte](($k -shr 8) -band 0xFF)
    $sp.Write($cf, 0, 8)
    $okAck = $false
    $deadline = (Get-Date).AddSeconds(2)
    while ((Get-Date) -lt $deadline) {
        try { $b = $sp.ReadByte() } catch { $b = -1 }
        if ($b -eq 0x06) { $okAck = $true; break }
        if ($b -eq 0x16) { break }
    }
    if (-not $okAck) { "切波特率失败(固件没回 ACK)"; $sp.Close(); exit 1 }
    Start-Sleep -Milliseconds 150
    $sp.Close()
    $sp.BaudRate = $TargetBaud
    $sp.Open()
    Start-Sleep -Milliseconds 200
    $sp.DiscardInBuffer()
    "已切到 {0} bps" -f $TargetBaud
}

# ---- 链路检测(ping): 发 [B5 5A D0 FF FF FF 00 00], 固件回 0x06 ----
#   固件不再发心跳, 由 PC 主动探测; 收不到就说明线/波特率/固件有问题, 直接退出
function Send-Ping([int]$timeoutMs) {
    $pf = New-Object byte[] 8
    $pf[0] = 0xB5; $pf[1] = 0x5A; $pf[2] = 0xD0; $pf[3] = 0xFF; $pf[4] = 0xFF; $pf[5] = 0xFF; $pf[6] = 0; $pf[7] = 0
    $sp.DiscardInBuffer()
    $sp.Write($pf, 0, 8)
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        try { $b = $sp.ReadByte() } catch { $b = -1 }
        if ($b -eq 0x06) { return $true }
    }
    return $false
}
$conn = $false
for ($t = 1; $t -le 3 -and -not $conn; $t++) { $conn = Send-Ping 1500 }
if (-not $conn) { "链路无响应(ping 没回 0x06): 检查串口线/波特率/固件是否在烧录模式"; $sp.Close(); exit 1 }
$ver = -1
try { $ver = $sp.ReadByte() } catch { }
if ($ver -ge 0) { "链路正常(ping -> 0x06), 固件版本 0x{0:X2}" -f $ver } else { "链路正常(ping -> 0x06), 无版本号(老固件)" }
if ($Ping) { $sp.Close(); exit 0 }

# ---- 整片块擦除(64KB/块): 先擦干净, 之后每帧只写不擦 ----
if (-not $NoErase) {
    $blocks = [int][Math]::Ceiling($total / 65536.0)
    $ef = New-Object byte[] 8
    $ef[0] = 0xB5; $ef[1] = 0x5A; $ef[2] = 0xE0; $ef[3] = 0xFF; $ef[4] = 0xFF; $ef[5] = 0xFF
    $ef[6] = [byte]($blocks -band 0xFF); $ef[7] = [byte](($blocks -shr 8) -band 0xFF)
    $sp.Write($ef, 0, 8)
    $okE = $false
    $deadline = (Get-Date).AddSeconds(120)          # 8MB 约 20s, 留足余量
    while ((Get-Date) -lt $deadline) {
        try { $b = $sp.ReadByte() } catch { $b = -1 }
        if ($b -eq 0x06) { $okE = $true; break }
        if ($b -eq 0x16) { break }
    }
    if (-not $okE) { "整片擦除失败(固件没回 ACK)"; $sp.Close(); exit 1 }
    "已块擦除 {0} 个 64KB 块 ({1:N1} MB)" -f $blocks, ($blocks * 65536 / 1MB)
}

$data = [IO.File]::ReadAllBytes($bin)
$pos  = [int]$StartByte

# ---- 流式模式: 分块(1MB)连续发, MCU 用 DMA 收、CPU 同时写 Flash(重叠) ----
#   头 13 字节: B5 5A 02 | addr(4,LE) | len(4,LE) | crc16(2,LE)
#   握手: 发完头等 0x06(可以发数据了) -> 连发 len 字节 -> 再等 0x06(写完)
if ($Stream) {
    $CHUNK  = $ChunkKB * 1KB                                 # 每块大小, 失败只重发这一块
    $padlen = [int]($data.Length + ((4096 - $data.Length % 4096) % 4096))
    $buf = New-Object byte[] $padlen
    [Array]::Copy($data, 0, $buf, 0, $data.Length)
    for ($i = $data.Length; $i -lt $padlen; $i++) { $buf[$i] = 0xFF }   # 末尾补 0xFF 对齐 4KB

    function Wait-Ack([int]$timeoutMs) {
        $deadline = (Get-Date).AddMilliseconds($timeoutMs)
        while ((Get-Date) -lt $deadline) {
            try { $b = $sp.ReadByte() } catch { $b = -1 }
            if ($b -eq 0x06) { return 1 }
            if ($b -eq 0x15) { return 2 }
            if ($b -eq 0x16) { return 3 }
        }
        return 0
    }

    $swAll = [Diagnostics.Stopwatch]::StartNew()
    $addr = 0
    while ($addr -lt $padlen) {
        $n   = [int][Math]::Min([long]$CHUNK, [long]($padlen - $addr))
        $crc = Crc16 $buf $addr $n
        $ok  = $false
        for ($try = 1; $try -le $Retry -and -not $ok; $try++) {
            $hdr = New-Object byte[] 13
            $hdr[0] = 0xB5; $hdr[1] = 0x5A; $hdr[2] = 0x02
            $hdr[3] = [byte]($addr -band 0xFF)
            $hdr[4] = [byte](($addr -shr 8) -band 0xFF)
            $hdr[5] = [byte](($addr -shr 16) -band 0xFF)
            $hdr[6] = [byte](($addr -shr 24) -band 0xFF)
            $hdr[7] = [byte]($n -band 0xFF)
            $hdr[8] = [byte](($n -shr 8) -band 0xFF)
            $hdr[9] = [byte](($n -shr 16) -band 0xFF)
            $hdr[10] = [byte](($n -shr 24) -band 0xFF)
            $hdr[11] = [byte]($crc -band 0xFF)
            $hdr[12] = [byte](($crc -shr 8) -band 0xFF)

            $sp.DiscardInBuffer()
            $sp.Write($hdr, 0, 13)
            $r = Wait-Ack 3000                                # 等 MCU 说"可以发数据了"
            if ($r -eq 2) { "  块 @ {0:X6} 头被拒(0x15)" -f $addr; continue }
            if ($r -eq 3) { "  块 @ {0:X6} 头被拒(0x16: 地址未擦除/长度不对)" -f $addr; break }
            if ($r -eq 0) { "  块 @ {0:X6} 等不到 ready(0x06)" -f $addr; continue }

            for ($p = 0; $p -lt $n; $p += 4096) { $sp.Write($buf, $addr + $p, 4096) }
            $r = Wait-Ack ([int]($n / 1024 / 100 * 1000 + 5000))   # 1MB 约 8s, 留余量
            if ($r -eq 1) { $ok = $true }
            elseif ($r -eq 2) {
                $st = -1
                $dbg = New-Object System.Collections.ArrayList
                try { $st = $sp.ReadByte() } catch { }
                for ($q = 0; $q -lt 20; $q++) { try { [void]$dbg.Add($sp.ReadByte()) } catch { } }
                $why = switch ($st) {
                    1 { '缓冲溢出(写入慢于接收)' }
                    2 { '收到的长度不够(掉数据)' }
                    3 { 'CRC 不符' }
                    default { "未知($st)" }
                }
                "  块 @ {0:X6} 失败: {1}" -f $addr, $why
                if ($dbg.Count -ge 20) {
                    $mcuCrc = ([int]$dbg[0]) -bor ([int]$dbg[1] -shl 8)
                    $pcCrc  = ([int]$dbg[2]) -bor ([int]$dbg[3] -shl 8)
                    $b0 = (($dbg[4..11])  | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
                    $b1 = (($dbg[12..19]) | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
                    $e0 = (($buf[$addr..($addr+7)]) | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
                    $e1 = (($buf[($addr+2048)..($addr+2055)]) | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
                    "    MCU CRC={0:X4}  头里的 CRC={1:X4}  PC 算的 CRC={2:X4}" -f $mcuCrc, $pcCrc, $crc
                    "    块0 缓冲: $b0"
                    "    块0 期望: $e0"
                    "    块1 缓冲: $b1"
                    "    块1 期望: $e1"
                }
            }
            else { "  块 @ {0:X6} 等不到写完 ACK" -f $addr }
        }
        if (-not $ok) { $sp.Close(); "流式烧录失败(块 @ {0:X6})" -f $addr; exit 1 }

        $addr += $n
        $kbps = $addr / $swAll.Elapsed.TotalSeconds / 1024
        "  {0:N0} / {1:N0} 字节  ({2:N1} KB/s)" -f $addr, $padlen, $kbps
    }
    $swAll.Stop()
    $sp.Close()
    "完成(流式): {0:N0} 字节, 用时 {1:N1}s, 平均 {2:N1} KB/s" -f $padlen, $swAll.Elapsed.TotalSeconds, ($padlen / $swAll.Elapsed.TotalSeconds / 1024)
    exit 0
}
$frames = 0
$fail = 0
$swAll = [Diagnostics.Stopwatch]::StartNew()

while ($pos -lt $total) {
    $n = [int][Math]::Min([long]$FRAME_SIZE, [long]($total - $pos))
    if ($Count -gt 0 -and $frames -ge $Count) { break }

    # 帧长 = 8 字节头(2 帧头 + 4 地址 + 2 长度) + n 数据 + 2 CRC
    $frame = New-Object byte[] (10 + $n)
    $frame[0] = 0xB5; $frame[1] = 0x5A
    $frame[2] = [byte]($pos -band 0xFF)
    $frame[3] = [byte](($pos -shr 8) -band 0xFF)
    $frame[4] = [byte](($pos -shr 16) -band 0xFF)
    $frame[5] = [byte](($pos -shr 24) -band 0xFF)
    $frame[6] = [byte]($n -band 0xFF)
    $frame[7] = [byte](($n -shr 8) -band 0xFF)
    [Array]::Copy($data, $pos, $frame, 8, $n)
    $crc = Crc16 $data $pos $n
    $frame[8 + $n] = [byte]($crc -band 0xFF)
    $frame[9 + $n] = [byte](($crc -shr 8) -band 0xFF)

    $ok = $false
    $seen = @()
    for ($try = 1; $try -le $Retry -and -not $ok; $try++) {
        $sp.DiscardInBuffer()
        $sp.Write($frame, 0, $frame.Length)
        $deadline = (Get-Date).AddMilliseconds($AckTimeoutMs)
        while ((Get-Date) -lt $deadline) {
            try { $b = $sp.ReadByte() } catch { $b = -1 }
            if ($b -lt 0) { continue }
            if ($b -eq 0x06) { $ok = $true; break }
            if ($b -eq 0x15 -or $b -eq 0x16) { $seen += ('{0:X2}' -f $b); continue }   # 记下, 继续收(调试字节在后)
            $seen += ('{0:X2}' -f $b)      # 其它(心跳等)记下来
        }
    }
    if (-not $ok) {
        $fail++
        $hint = if ($seen.Count -eq 0) { '一个字节都没收到' } else { '收到: ' + ($seen -join ' ') }
        "  帧 @ {0:X6} 失败({1}), 已重试 {2} 次" -f $pos, $hint, $Retry
        if ($fail -ge 3) { "连续失败, 停止"; break }
    } else {
        $fail = 0
    }

    $pos += $n
    $frames++
    if (($frames % 25) -eq 0) {
        $kbps = ($pos - $StartByte) / $swAll.Elapsed.TotalSeconds / 1024
        "  {0:N0} / {1:N0} 字节  ({2:N1} KB/s)" -f $pos, $total, $kbps
    }
}

$sp.Close()
$sec = $swAll.Elapsed.TotalSeconds
$bytes = $pos - $StartByte
"完成: {0:N0} 字节, {1} 帧, 用时 {2:N1}s, 平均 {3:N1} KB/s" -f $bytes, $frames, $sec, ($bytes / $sec / 1024)
