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
    [switch]$NoErase          # 跳过"整片块擦除"(默认会先擦)
)

$ErrorActionPreference = 'Stop'

if ($BinPath -eq '') {
    $BinPath = Join-Path $PSScriptRoot 'cache\imported_video.bin'
}
$bin = (Get-Item -LiteralPath $BinPath).FullName
$total = (Get-Item -LiteralPath $bin).Length
"源文件: {0}  ({1:N0} 字节)" -f $bin, $total

$FRAME_SIZE = 12288

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
