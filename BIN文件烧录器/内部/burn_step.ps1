# burn_step.ps1 - 分步烧录(供"烧录面板"网页调用)
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File burn_step.ps1 -Step font|btn|game|video|auto
#            [-BinPath <视频.bin>] [-WithVideo] [-DryRun] [-SwdSpeed 8000]
#
# 各步含义(与规则文档 §32.3 的步骤号对应):
#   font  = 字库/图片资源(FLASH_DATA_STORE_MODE)   -> 外部 Flash 末尾
#   btn   = 英文/撤销按钮图 + 菜单字模(BTN_IMG_STORE_MODE) -> 外部 Flash 保留区
#   game  = 游戏程序(GAME: 三个烧录宏全关)          -> MCU 内部 Flash
#   video = 视频数据块(VIDEO_CHUNK_STORE_MODE)     -> 外部 Flash 地址 0 (内部调用 burn_all.ps1)
#   auto  = font -> btn -> [video] -> game   (video 只在 -WithVideo 且给了 bin 时才做)
#
# 输出约定(网页服务按行解析): 以 "STEP_OK <name>" / "STEP_FAIL <name> <原因>" 结尾
# 退出码: 0 = 成功, 1 = 失败
#
# 注意: 三步烧录固件都只做"编译 + SWD 烧写", 不自动把游戏固件烧回去;
#       游戏固件由 game 那一步负责(所以单独烧 font/btn 后, 记得再点"程序烧录")。
param([string]$Step = 'auto', [string]$BinPath = '', [switch]$WithVideo, [switch]$DryRun, [int]$SwdSpeed = 8000)

$ErrorActionPreference = 'Continue'

# ---------- 路径: 脚本可能放在工程内, 也可能在外面 ----------
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
$proj = Find-Proj $PSScriptRoot
if ($proj -eq '') {
    Write-Host 'STEP_FAIL init 找不到工程根(应含 SPI\SPI_LCD\User\src)'
    exit 1
}

$cfg     = Join-Path $proj 'SPI\SPI_LCD\User\inc\config.h'
$obj     = Join-Path $proj 'SPI\SPI_LCD\obj'
$elf     = Join-Path $obj 'SPI_LCD.elf'
$logFile = Join-Path $obj 'burn_log.txt'
$switchScript = Join-Path $PSScriptRoot 'switch_mode.ps1'
$burnAll = Join-Path $PSScriptRoot 'burn_all.ps1'
$uartBurn = Join-Path $PSScriptRoot 'uart_burn.ps1'   # 视频: 串口流式烧录(快约 10 倍)
$buildScript = Join-Path $proj '编译与下载.ps1'   # 编译复用根目录脚本(工具链/开关只维护一处)

$mrs     = 'D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32'
$openocd = Join-Path $mrs 'components\WCH\OpenOCD\OpenOCD\bin\openocd.exe'
$ocfg    = Join-Path $mrs 'components\WCH\OpenOCD\OpenOCD\bin\wch-riscv.cfg'
$gcc     = Join-Path $mrs 'components\WCH\Toolchain\RISC-V Embedded GCC12\bin'
$mkbin   = Join-Path $mrs 'others\Build_Tools\Make\bin'
$nm      = Join-Path $gcc 'riscv-wch-elf-nm.exe'
$env:Path = "$gcc;$mkbin;" + $env:Path
$latin = [Text.Encoding]::GetEncoding(28591)      # byte<->char 1:1, 保住 config.h 原编码

function Log($m) { Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }

# ---------- config.h 宏开关 ----------
function SetMacro([string]$name, [bool]$on) {
    $raw = $latin.GetString([IO.File]::ReadAllBytes($cfg))
    $n   = [regex]::Escape($name)
    if ($on) { $src = '(?m)^//\s*(#define\s+' + $n + ')'; $rep = '$1' }
    else     { $src = '(?m)^(?!//)(#define\s+' + $n + ')'; $rep = '//$1' }
    $new = [regex]::Replace($raw, $src, $rep)
    if ($new -ne $raw) { [IO.File]::WriteAllBytes($cfg, $latin.GetBytes($new)); return $true }
    return $false
}

# ---------- 符号地址 / 编译 / 回读 ----------
function SymAddr([string]$name) {
    if (-not (Test-Path $nm)) { return '' }
    $h = (& $nm $elf 2>$null) | Select-String -Pattern ('\s' + $name + '$') | Select-Object -First 1
    if (-not $h) { return '' }
    return (($h.Line -split '\s+') | Where-Object { $_ } | Select-Object -First 1)
}

# 起子 PowerShell 跑脚本: -WindowStyle Hidden 不弹窗口; 数组形式的参数不会自动加引号,
# 带空格的路径(本工程路径里有空格!)会被切断 -> 这里统一手工加引号
function Start-PsScript {
    param([string]$Script, [string[]]$ExtraArgs = @(), [switch]$Wait)
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $Script))
    foreach ($x in $ExtraArgs) {
        if ($x -match '\s') { $a += ('"{0}"' -f $x) } else { $a += $x }
    }
    if ($Wait) { return (Start-Process powershell -ArgumentList $a -Wait -PassThru -WindowStyle Hidden) }
    return (Start-Process powershell -ArgumentList $a -PassThru -WindowStyle Hidden)
}
function Build([string]$tag) {
    # 复用根目录 编译与下载.ps1 的 rebuild 模式(清 + 编译, 不烧写):
    # 工具链(GCC12)与编译开关只在那边维护一处, 避免两边不一致(见备忘录 §52/§54)
    if (-not (Test-Path $buildScript)) { Log ("找不到编译脚本: {0}" -f $buildScript); return 1 }
    $o = & powershell -NoProfile -ExecutionPolicy Bypass -File "$buildScript" rebuild 2>&1 | Out-String
    $ec = $LASTEXITCODE
    if ($ec -ne 0) {
        Log ("编译失败({0})" -f $tag)
        Log ("$o")
    } else {
        Log ("编译成功({0})" -f $tag)
        ($o -split "`n" | Where-Object { $_ -match 'Flash 已用' } | Select-Object -First 1) | ForEach-Object { Log ($_.Trim()) }
    }
    return $ec
}

function ReadWords([string]$addr, [int]$n, [string]$tag) {
    $tmp = Join-Path $obj 'step_rd.txt'
    $idx = (0..($n - 1) | ForEach-Object { "`$m($_)" }) -join ','
    & $openocd -f $ocfg -c "adapter speed $SwdSpeed" -c 'chip_id CH32V20x' -c 'init' `
        -c "mem2array m 32 0x$addr $n" -c "echo $tag=$idx" -c 'shutdown' *> $tmp
    $txt = $latin.GetString([IO.File]::ReadAllBytes($tmp)) -replace "`0", ''
    $hit = [regex]::Match($txt, ($tag + '=(0x[0-9a-fA-F]+(?:,0x[0-9a-fA-F]+){' + ($n - 1) + '})'))
    if (-not $hit.Success) { return $null }
    return ($hit.Groups[1].Value -split ',' | ForEach-Object { [Convert]::ToUInt32($_.Substring(2), 16) })
}

# 串口与 SWD 并线时, 固件驱动 PA9(UART TX, 空闲高) 会把 SDI 顶死 -> 先发 7F 7F 7F
# 让 UART 烧录固件把 PA9/PA10 设成浮空并重新打开 SWD(等于"软件上拔掉 UART")
function Send-SwdRelease {
    $dev = (Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
            Where-Object { $_.Description -match 'WCH-Link' } | Select-Object -First 1)
    if (-not $dev) { return }
    foreach ($b in @(1200000, 1500000, 460800)) {
        try {
            $sp = New-Object System.IO.Ports.SerialPort($dev.DeviceID, $b)
            $sp.ReadTimeout = 150; $sp.WriteTimeout = 300
            $sp.Open(); Start-Sleep -Milliseconds 80
            $sp.Write([byte[]](0x7F,0x7F,0x7F), 0, 3)
            Start-Sleep -Milliseconds 120
            $sp.Close()
        } catch { }
        Start-Sleep -Milliseconds 40
    }
}

# 等 SWD 连上(设备待机时要点一下屏幕唤醒); 200 次 x 1.2s
function WaitConn([int]$max = 200) {
    for ($k = 1; $k -le $max; $k++) {
        if ($k -eq 1) { Send-SwdRelease }
        & $openocd -f $ocfg -c "adapter speed $SwdSpeed" -c 'chip_id CH32V20x' -c 'init' -c 'shutdown' *> (Join-Path $obj 'step_probe.txt')
        $t = $latin.GetString([IO.File]::ReadAllBytes((Join-Path $obj 'step_probe.txt')))
        if ($t -notmatch 'failed to connect') {
            if ($k -gt 1) { Log ("SWD 已连上(第 {0} 次探测)" -f $k) }
            return $true
        }
        if ($k -eq 1) { Log '等待 SWD 连接... 设备若是待机, 请点一下屏幕' }
        Start-Sleep -Milliseconds 1200
    }
    return $false
}

# 烧录固件 + 清 magic(防止读到上一块板/上次运行残留的 RAM) + 复位运行
function FlashEmf([string]$magicAddr, [bool]$clearMagic) {
    $args = @('-f', $ocfg, '-c', "adapter speed $SwdSpeed", '-c', 'chip_id CH32V20x',
              '-c', "program {$elf} verify")
    if ($clearMagic -and $magicAddr -ne '') { $args += @('-c', "mww 0x$magicAddr 0") }
    $args += @('-c', 'reset run', '-c', 'shutdown')
    $o = & $openocd @args 2>&1 | Out-String
    return ($o -match 'Programming Finished')
}

# 轮询某个进度结构: magic 通过且 result != 0 才返回
function WaitProg([string]$addr, [int]$words, [uint32]$magic, [int]$timeoutSec = 120) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $v = ReadWords $addr $words 'PG'
        if ($v -and $v[0] -eq $magic -and $v[($words - 1)] -ne 0) { return $v }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

# ---------- 三个"烧录固件"步骤: 切宏 -> 编译 -> 烧写 -> 回读校验 ----------
function Step-Font {
    Log '===== 步骤 1/3 字库/图片资源 ====='
    if (-not $DryRun) {
        Start-PsScript -Script $switchScript -ExtraArgs @('game') -Wait | Out-Null   # 视频宏关 + 去 -flto + make clean
        SetMacro 'UART_STORE_MODE' $false | Out-Null
        SetMacro 'FLASH_DATA_STORE_MODE' $true | Out-Null
        Log 'config.h: FLASH_DATA_STORE_MODE = 开(另两个宏已关)'
        if ((Build '字库') -ne 0) { return '编译失败' }
        $addr = SymAddr 'g_res_prog'
        if ($addr -eq '') { return '基准镜像里没有 g_res_prog 符号(模式宏不对?)' }
        if (-not (WaitConn)) { return 'SWD 连不上(设备待机?)' }
        if (-not (FlashEmf $addr $true)) { return 'SWD 烧写失败' }
        Log '已烧写待运行, 等 MCU 自己把资源写进外部 Flash...'
        $v = WaitProg $addr 6 0x52455331 180
        if (-not $v) { return '等不到 g_res_prog 结果(超时)' }
        if ($v[5] -ne 1) { return ("字库烧录失败: result={0} crc_fail={1}" -f $v[5], $v[4]) }
        Log ("资源 {0}/{1} 项已写入, CRC 全部通过" -f $v[2], $v[1])
    }
    Log 'STEP_OK font'
    return ''
}

function Step-Btn {
    Log '===== 步骤 2/3 按钮图 + 菜单字模 ====='
    if (-not $DryRun) {
        Start-PsScript -Script $switchScript -ExtraArgs @('game') -Wait | Out-Null
        SetMacro 'FLASH_DATA_STORE_MODE' $false | Out-Null
        SetMacro 'UART_STORE_MODE' $false | Out-Null
        SetMacro 'BTN_IMG_STORE_MODE' $true | Out-Null
        Log 'config.h: BTN_IMG_STORE_MODE = 开(另两个宏已关)'
        if ((Build '按钮图') -ne 0) { return '编译失败' }
        $addr = SymAddr 'g_btn_prog'
        if ($addr -eq '') { return '基准镜像里没有 g_btn_prog 符号(模式宏不对?)' }
        if (-not (WaitConn)) { return 'SWD 连不上(设备待机?)' }
        if (-not (FlashEmf $addr $true)) { return 'SWD 烧写失败' }
        Log '已烧写待运行, 等 MCU 自己写保留区...'
        $v = WaitProg $addr 4 0x42544E31 120
        if (-not $v) { return '等不到 g_btn_prog 结果(超时)' }
        if ($v[3] -ne 1) { return ("按钮图烧录失败: result={0}" -f $v[3]) }
        Log ("保留区 {0} 字节(英文导航 + 撤销中英 + 菜单字模) 已写入且 CRC 通过" -f $v[1])
    }
    Log 'STEP_OK btn'
    return ''
}

function Step-Game {
    Log '===== 步骤 3/3 游戏程序 ====='
    if (-not $DryRun) {
        Start-PsScript -Script $switchScript -ExtraArgs @('game') -Wait | Out-Null
        SetMacro 'FLASH_DATA_STORE_MODE' $false | Out-Null
        SetMacro 'UART_STORE_MODE' $false | Out-Null
        SetMacro 'BTN_IMG_STORE_MODE' $false | Out-Null
        Log 'config.h: 三个烧录宏全关(GAME 模式)'
        if ((Build '游戏') -ne 0) { return '编译失败' }
        if (-not (WaitConn)) { return 'SWD 连不上(设备待机?)' }
        $o = & $openocd -f $ocfg -c "adapter speed $SwdSpeed" -c 'chip_id CH32V20x' `
             -c "program {$elf} reset exit" 2>&1 | Out-String
        if ($o -notmatch 'Programming Finished') { return 'SWD 烧写失败' }
        Log '游戏固件已烧写并复位运行'
    }
    Log 'STEP_OK game'
    return ''
}

# ---------- 视频: UART 烧录固件 + 串口流式(8MB 约 70s), 比 SWD 分块快约 10 倍 ----------
function Step-Video {
    Log '===== 视频数据块(外部 Flash 地址 0, 串口流式) ====='
    if ($BinPath -eq '' -or -not (Test-Path -LiteralPath $BinPath)) {
        return '没有视频 bin(请先在面板上导入)'
    }
    Log ("视频源: {0}" -f $BinPath)
    if ($DryRun) { Log 'STEP_OK video'; return '' }

    # 1) 切到 UART 烧录固件: 只开 UART_STORE_MODE, 其余烧录宏全关
    SetMacro 'FLASH_DATA_STORE_MODE' $false | Out-Null
    SetMacro 'BTN_IMG_STORE_MODE' $false | Out-Null
    SetMacro 'VIDEO_CHUNK_STORE_MODE' $false | Out-Null
    SetMacro 'UART_STORE_MODE' $true | Out-Null
    Log 'config.h: UART_STORE_MODE = 开(其余烧录宏已关)'
    if ((Build '串口烧录') -ne 0) { return '编译失败' }

    # 2) 烧进 MCU(SWD), 复位后就在跑串口烧录固件
    #    用 program ... reset exit(先复位再写, 最稳); 失败重发 7F 再来
    if (-not (WaitConn)) { return 'SWD 连不上(设备待机?)' }
    $flashed = $false
    for ($t = 1; $t -le 3 -and -not $flashed; $t++) {
        Send-SwdRelease
        $o = & $openocd -f $ocfg -c "adapter speed $SwdSpeed" -c 'chip_id CH32V20x' `
             -c "program {$elf} reset exit" 2>&1 | Out-String
        if ($o -match 'Programming Finished') { $flashed = $true }
        else { Log ("  烧写第 {0} 次失败, 重试" -f $t); Start-Sleep -Milliseconds 500 }
    }
    if (-not $flashed) { return 'SWD 烧写失败(请按一下复位再试)' }
    Log '串口烧录固件已烧写, 等它启动...'
    Start-Sleep -Seconds 6

    # 3) 串口流式写外部 Flash: 1MB 分块, 每块独立 CRC/重试, 边收边写
    #    ★ 直接在同一个进程里调用: 起子进程(Start-Process + 输出重定向)实测会让 MCU
    #      在第一块之后就失联, 同进程调用稳定
    & $uartBurn -Stream -Baud 1200000 -ChunkKB 1024 -BinPath $BinPath 2>&1 |
        ForEach-Object { Log ("  视频 " + $_) }
    if ($LASTEXITCODE -ne 0) { return '串口流式烧录失败(详见上面的输出)' }
    Log 'STEP_OK video'
    return ''
}

# ---------- 主流程 ----------
Log ("烧录步骤: {0}{1}" -f $Step, $(if ($DryRun) { ' (DryRun 只列步骤不烧写)' } else { '' }))

$fail = ''
switch ($Step) {
    'font'  { $fail = Step-Font }
    'btn'   { $fail = Step-Btn }
    'game'  { $fail = Step-Game }
    'video' { $fail = Step-Video }
    'auto'  {
        $fail = Step-Font
        if ($fail -eq '') { $fail = Step-Btn }
        if ($fail -eq '' -and $WithVideo) { $fail = Step-Video }
        if ($fail -eq '') { $fail = Step-Game }
    }
    default { $fail = "未知步骤: $Step" }
}

if ($fail -ne '') {
    Log ("失败: {0}" -f $fail)
    Log ("STEP_FAIL {0} {1}" -f $Step, $fail)
    exit 1
}
Log '全部完成'
exit 0
