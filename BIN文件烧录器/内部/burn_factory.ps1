# burn_factory.ps1 - 出厂全量烧录: 字库/图片 → 视频 → 游戏程序 (一块板一条命令)
#
# 用法:
#   powershell -File burn_factory.ps1                    # 烧 1 块板
#   powershell -File burn_factory.ps1 -Boards 5          # 连烧 5 块板(每块之间停下来等你换板)
#   powershell -File burn_factory.ps1 -Refresh           # 忽略缓存, 强制重编
#   powershell -File burn_factory.ps1 -BinPath "D:\xx\压缩数据1~4.bin"
#   powershell -File burn_factory.ps1 -ProjPath "D:\xx\03-...-触摸屏"
#
# 流程:
#   ① 首次(或源码变了): 编译并缓存一份"纯烧写用"的字库固件与游戏固件; 视频由 burn_all.ps1 自己编基准镜像
#      —— 之后每块板都是纯烧写, 不再编译(省掉每块板 2 次 make clean + 完整重编, 约 3 分钟)
#   ② 每块板: 烧字库固件 → SWD 回读 g_res_prog 等 result → 烧视频 57 块(自带逐块 CRC 校验) → 烧游戏固件
#   ③ 三步的 SWD 连接都自动重试: 设备待机时连不上, 脚本会提示"点一下屏幕"
#
# 缓存失效保护: 只要 SPI/ 下有 .c/.h 比缓存新(或加了 -Refresh), 就自动重新编译
param([int]$Boards = 1, [switch]$Refresh, [string]$BinPath = '', [string]$ProjPath = '', [int]$SwdSpeed = 8000)

$ErrorActionPreference = 'Continue'

# ---------- 工具路径 ----------
$mrs  = "D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32"
$openocd = Join-Path $mrs "components\WCH\OpenOCD\OpenOCD\bin\openocd.exe"
$ocfg    = Join-Path $mrs "components\WCH\OpenOCD\OpenOCD\bin\wch-riscv.cfg"
$gcc     = Join-Path $mrs "components\WCH\Toolchain\RISC-V Embedded GCC\bin"
$mkbin   = Join-Path $mrs "others\Build_Tools\Make\bin"
$nm      = Join-Path $gcc "riscv-none-embed-nm.exe"
$env:Path = "$gcc;$mkbin;" + $env:Path
$latin = [Text.Encoding]::GetEncoding(28591)   # byte<->char 1:1, 保住 config.h 的原编码

function Fail($msg) { Write-Host $msg -ForegroundColor Red; exit 1 }
function Step($msg) { Write-Host $msg -ForegroundColor Cyan }
function Ok($msg)   { Write-Host $msg -ForegroundColor Green }

# ---------- 定位工程(含 SPI\SPI_LCD\User\src); 脚本放哪都行 ----------
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
if ($ProjPath -ne '') { $proj = (Get-Item -LiteralPath $ProjPath).FullName }
else {
    $proj = Find-Proj $PSScriptRoot
    if ($proj -eq '') { $proj = Find-Proj-Sibling $PSScriptRoot }
}
if ($proj -eq '' -or -not (Test-Path $proj)) { Fail "找不到工程根(应含 SPI\SPI_LCD\User\src): 用 -ProjPath 指定" }

$cfg     = Join-Path $proj 'SPI\SPI_LCD\User\inc\config.h'
$obj     = Join-Path $proj 'SPI\SPI_LCD\obj'
$elf     = Join-Path $obj 'SPI_LCD.elf'
$cache   = Join-Path $PSScriptRoot 'cache'
$elfData = Join-Path $cache 'factory_data.elf'
$elfGame = Join-Path $cache 'factory_game.elf'
$log     = Join-Path $obj 'factory_log.txt'
$burnAll = Join-Path $PSScriptRoot 'burn_all.ps1'
$switch  = Join-Path $PSScriptRoot 'switch_mode.ps1'

# 源视频 .bin: -BinPath 优先; 否则 脚本目录 -> 工程内 数据\娜娜莉 -> 外层 数据\娜娜莉
if ($BinPath -ne '') { $bin = (Get-Item -LiteralPath $BinPath).FullName }
else {
    $bin = ''
    $outerData = Join-Path (Split-Path (Split-Path $proj -Parent) -Parent) '数据\娜娜莉'
    foreach ($c in @($PSScriptRoot, (Join-Path $proj '数据\娜娜莉'), $outerData)) {
        if ($c -and (Test-Path $c)) {
            $f = Get-ChildItem -Path $c -Filter *.bin | Select-Object -First 1
            if ($f) { $bin = $f.FullName; break }
        }
    }
    if ($bin -eq '') { Fail "找不到源视频 .bin: 用 -BinPath <路径> 指定" }
}

# ---------- config.h 模式宏(只动 FLASH_DATA_STORE_MODE; VIDEO_CHUNK_STORE_MODE 交给 switch_mode.ps1) ----------
function Set-DataMacro([bool]$on) {
    $raw = $latin.GetString([IO.File]::ReadAllBytes($cfg))
    $src = if ($on) { '(?m)^//\s*(#define\s+FLASH_DATA_STORE_MODE)' } else { '(?m)^(?!//)(#define\s+FLASH_DATA_STORE_MODE)' }
    $rep = if ($on) { '$1' } else { '//$1' }
    $new = [regex]::Replace($raw, $src, $rep)
    if ($new -ne $raw) { [IO.File]::WriteAllBytes($cfg, $latin.GetBytes($new)); return $true }
    return $false
}
function Ensure-VideoMode() {
    $raw = $latin.GetString([IO.File]::ReadAllBytes($cfg))
    return ($raw -match '(?m)^#define\s+VIDEO_CHUNK_STORE_MODE')
}
function New-Build([string]$tag) {
    Set-Location $obj
    & make clean *> $null
    $o = & make all 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        # 偶发 GCC 内部错误(qsort checking failed): 清掉旧目标文件重编一次(与 编译与下载.ps1 同策略)
        Get-ChildItem -Path $obj -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -in '.o', '.d', '.map', '.lst' } | Remove-Item -Force
        $o = & make all 2>&1 | Out-String
    }
    if ($LASTEXITCODE -ne 0) { Fail "编译失败($tag)`n$o" }
    $sizeLine = ($o -split "`n" | Where-Object { $_ -match '^\s+FLASH:' } | Select-Object -First 1)
    Ok ("  编译成功({0}) {1}" -f $tag, "$sizeLine".Trim())
}

# ---------- SWD: 烧写(自动重试) + 回读 ----------
function Flash-Elf([string]$elfPath, [string]$clearSym = '') {
    $addr = ''
    if ($clearSym -ne '') { $addr = Get-SymAddr $elfPath $clearSym }
    # 200 次 × 约 3.3s ≈ 11 分钟: 批量烧板时操作员可能在换板/接线, 留足"点一下屏幕"的时间
    for ($i = 1; $i -le 200; $i++) {
        if ($addr -ne '') {
            # 核心要点: 编程后停在 halt, 先把 magic 清零再 run —— 否则 PC 可能读到上一块板/上一次
            # 烧录留在 RAM 里的 result(和本次一模一样), 冒充"烧完了"
            $a = @('-f', $ocfg, '-c', "adapter speed $SwdSpeed", '-c', 'chip_id CH32V20x',
                   '-c', "program {$elfPath} verify", '-c', "mww 0x$addr 0", '-c', 'reset run', '-c', 'shutdown')
        } else {
            $a = @('-f', $ocfg, '-c', "adapter speed $SwdSpeed", '-c', 'chip_id CH32V20x',
                   '-c', "program {$elfPath} reset exit")
        }
        $o = & $openocd @a 2>&1 | Out-String
        if ($o -match 'Programming Finished') { return $true }
        if ($i -eq 1 -or ($i % 10) -eq 0) {
            Write-Host ("  等待 SWD 连接(第 {0} 次)... 设备若是待机, 请点一下屏幕" -f $i) -ForegroundColor Yellow
        }
        Start-Sleep -Milliseconds 1500
    }
    return $false
}
function Get-SymAddr([string]$elfPath, [string]$name) {
    $h = (& $nm $elfPath 2>$null) | Select-String -Pattern ("\s" + $name + "$") | Select-Object -First 1
    if (-not $h) { return '' }
    return (($h.Line -split '\s+') | Where-Object { $_ } | Select-Object -First 1)
}
# 缓存固件身份自检: 两个烧录宏同时定义时, main.c 只会走视频那条 → 字库固件会被编成视频固件 ✗
# 用符号特征把它挡住(字库固件必须有 g_res_prog; 游戏固件里 g_video_chunk 应被 --gc-sections 回收)
function Assert-Sym([string]$elfPath, [string]$name, [bool]$mustExist) {
    $has = ((Get-SymAddr $elfPath $name) -ne '')
    if ($has -ne $mustExist) {
        $why = if ($mustExist) { '应该存在却没找到(模式宏不对?)' } else { '不应存在却出现了(模式宏不对?)' }
        Fail ("缓存固件自检失败: {0} 里的 {1} {2}" -f (Split-Path $elfPath -Leaf), $name, $why)
    }
}
function Read-Words([string]$addr, [int]$n, [string]$tag) {
    $tmp = Join-Path $obj 'factory_rd.txt'
    $idx = (0..($n - 1) | ForEach-Object { "`$m($_)" }) -join ','
    & $openocd -f $ocfg -c "adapter speed $SwdSpeed" -c 'chip_id CH32V20x' -c 'init' `
        -c "mem2array m 32 0x$addr $n" -c "echo $tag=$idx" -c 'shutdown' *> $tmp
    $txt = $latin.GetString([IO.File]::ReadAllBytes($tmp)) -replace "`0", ''
    $hit = [regex]::Match($txt, ($tag + '=(0x[0-9a-fA-F]+(?:,0x[0-9a-fA-F]+){' + ($n - 1) + '})'))
    if (-not $hit.Success) { return $null }
    return ($hit.Groups[1].Value -split ',' | ForEach-Object { [Convert]::ToUInt32($_.Substring(2), 16) })
}

# 等字库烧录结果: magic='RES1' + result(0未完成/1通过/2失败)
function Wait-ResProg([string]$elfPath) {
    $addr = Get-SymAddr $elfPath 'g_res_prog'
    if ($addr -eq '') { Write-Host "  × 缓存固件里找不到 g_res_prog 符号" -ForegroundColor Red; return $null }
    for ($i = 1; $i -le 120; $i++) {
        $v = Read-Words $addr 6 'RP'
        if ($v -and $v[0] -eq 0x52455331) {
            if ($v[5] -ne 0) {
                return [pscustomobject]@{ Pct = $v[3]; Index = $v[2]; Total = $v[1]; CrcFail = $v[4]; Result = $v[5] }
            }
        }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

# 烧视频并统计本次运行结果(读 burn_all 自己的日志)
function Burn-Video() {
    & powershell -ExecutionPolicy Bypass -File $burnAll -start 0 -BinPath $bin *> $null
    $l = Get-Content -LiteralPath (Join-Path $obj 'burn_log.txt') -Encoding UTF8
    $h = ($l | Select-String 'burn_all\(patch\) start=' | Select-Object -Last 1).LineNumber
    if (-not $h) { return [pscustomobject]@{ Blocks = 0; Errors = 999 } }
    $cur = $l[$h..($l.Count - 1)]
    return [pscustomobject]@{
        Blocks = ($cur | Select-String ' done ' | Measure-Object).Count
        Errors = ($cur | Select-String 'FAIL|MISMATCH|TIMEOUT').Count
    }
}

# ---------- 缓存: 按"源码内容指纹"判断是否需要重编 ----------
# ★ 不能用 mtime: 本脚本自己切模式会改 config.h, gen_video_chunk.py 每块都重写 video_chunk.c/h
#   —— 都会让"源码比缓存新"误判, 于是每块板都白重编一遍。
#   所以改成: 内容哈希 + 排除生成文件 + config.h 里两个模式宏归一化(切模式不算改动)。
function Get-SrcHash {
    $sb = New-Object Text.StringBuilder
    $files = Get-ChildItem -Path (Join-Path $proj 'SPI') -Recurse -Include *.c, *.h -File -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -notmatch '\\obj\\' -and $_.Name -notmatch '^video_chunk\.' } |
             Sort-Object FullName
    foreach ($f in $files) {
        $null = $sb.Append($f.FullName)
        $txt = $latin.GetString([IO.File]::ReadAllBytes($f.FullName))
        if ($f.Name -eq 'config.h') {
            $txt = [regex]::Replace($txt, '(?m)^\s*(?://)?\s*(#define\s+(?:FLASH_DATA_STORE_MODE|VIDEO_CHUNK_STORE_MODE))', '$1')
        }
        $null = $sb.Append($txt)
    }
    $md5 = [Security.Cryptography.MD5]::Create()
    return ([BitConverter]::ToString($md5.ComputeHash([Text.Encoding]::UTF8.GetBytes($sb.ToString())))).Replace('-', '')
}
function Test-Cache([string]$elfPath) {
    if ($Refresh) { return $false }
    if (-not (Test-Path $elfPath)) { return $false }
    $stamp = "$elfPath.hash"
    if (-not (Test-Path $stamp)) { return $false }
    return ((Get-Content -LiteralPath $stamp -Raw).Trim() -eq (Get-SrcHash))
}
function Save-CacheStamp([string]$elfPath) {
    Set-Content -LiteralPath "$elfPath.hash" -Value (Get-SrcHash) -Encoding ASCII
}

if (-not (Test-Path $cache)) { New-Item -ItemType Directory -Path $cache | Out-Null }
$needData = -not (Test-Cache $elfData)
$needGame = -not (Test-Cache $elfGame)

if ($needData -or $needGame) {
    Step "== 编译阶段(只做一次; 后续板子纯烧写) =="
    # 字库固件要求"视频宏关 + 字库宏开"。视频宏若开着, 必须先关掉:
    # 两个烧录宏同时定义时 main.c 的 #if/#elif 只会走视频分支 → 编出来的根本不是字库固件 ✗
    if (Ensure-VideoMode) { & powershell -ExecutionPolicy Bypass -File $switch game *> $null }
    if ($needData) {
        Set-DataMacro $true | Out-Null
        New-Build '字库'
        Assert-Sym $elf 'g_res_prog' $true       # 自检: 确认这真是字库固件
        Assert-Sym $elf 'g_video_chunk' $false
        Copy-Item $elf $elfData -Force
        Save-CacheStamp $elfData
        Ok ("  已缓存: {0} ({1:N0} B)" -f (Split-Path $elfData -Leaf), (Get-Item $elfData).Length)
    }
    if ($needGame) {
        Set-DataMacro $false | Out-Null       # ★ 必须在编译前关掉字库宏, 否则编出来的"游戏固件"其实是字库固件
        & powershell -ExecutionPolicy Bypass -File $switch game *> $null
        New-Build '游戏'
        Assert-Sym $elf 'g_res_prog' $false   # 自检: 游戏固件绝不能有 g_res_prog(那是字库固件的标志)
        Assert-Sym $elf 'g_video_chunk' $false
        Copy-Item $elf $elfGame -Force
        Save-CacheStamp $elfGame
        Ok ("  已缓存: {0} ({1:N0} B)" -f (Split-Path $elfGame -Leaf), (Get-Item $elfGame).Length)
    }
} else {
    Step ("== 使用缓存固件 ==")
    Ok ("  字库: {0} ({1})" -f (Split-Path $elfData -Leaf), (Get-Item $elfData).LastWriteTime.ToString('MM-dd HH:mm'))
    Ok ("  游戏: {0} ({1})" -f (Split-Path $elfGame -Leaf), (Get-Item $elfGame).LastWriteTime.ToString('MM-dd HH:mm'))
}
# ★ 两个收尾动作, 必须"无论走哪条分支"都执行:
#   ① 字库宏关回去 —— 只有编译阶段会开它; 若上次运行中途断在字库模式(或游戏缓存命中时跳过编译),
#      漏掉这步就会让 config.h 停在字库模式, 下次谁一编译就烧错了 ✗
#   ② config.h 停在"视频烧录模式" —— burn_all.ps1 要用它编基准镜像; 否则它会把游戏固件当基准打补丁 ✗
Set-DataMacro $false | Out-Null
if (-not (Ensure-VideoMode)) { & powershell -ExecutionPolicy Bypass -File $switch burn *> $null }
Write-Host ("  视频源: {0}" -f $bin)

# ---------- 每块板 ----------
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$nBoard = 0
for ($b = 1; $b -le $Boards; $b++) {
    if ($Boards -gt 1 -and $b -gt 1) {
        Write-Host ""
        Read-Host ("请换上第 {0} 块板(接好 SWD), 回车继续" -f $b) | Out-Null
    }
    $t0 = [DateTime]::Now
    Step ("===== 第 {0}/{1} 块板  {2} =====" -f $b, $Boards, $t0.ToString('HH:mm:ss'))
    $okAll = $true

    Step "  [1/3] 字库/图片 -> 外部 Flash 末尾"
    if (-not (Flash-Elf $elfData 'g_res_prog')) { Write-Host "  × 字库固件烧不进去" -ForegroundColor Red; $okAll = $false }
    else {
        $r = Wait-ResProg $elfData
        if ($null -eq $r) { Write-Host "  × 等不到字库烧录结果(超时)" -ForegroundColor Red; $okAll = $false }
        elseif ($r.Result -eq 1) { Ok ("  ? 字库 {0}/{1} 项写入且 CRC 全部通过" -f $r.Index, $r.Total) }
        else { Write-Host ("  × 字库有 CRC 失败: {0} 项(共 {1} 项)" -f $r.CrcFail, $r.Total) -ForegroundColor Red; $okAll = $false }
    }

    Step "  [2/3] 视频 57 块 -> 外部 Flash 地址 0"
    if (-not $okAll) { Write-Host "  跳过(前一步已失败)" -ForegroundColor Yellow }
    else {
        $v = Burn-Video
        if ($v.Errors -eq 0 -and $v.Blocks -gt 0) { Ok ("  ? 视频 {0} 块全部通过" -f $v.Blocks) }
        else { Write-Host ("  × 视频异常 {0} 处(完成 {1} 块)," -f $v.Errors, $v.Blocks) -ForegroundColor Red; $okAll = $false }
    }

    Step "  [3/3] 游戏程序 -> 内部 Flash"
    if (-not $okAll) { Write-Host "  跳过(前一步已失败)" -ForegroundColor Yellow }
    elseif (-not (Flash-Elf $elfGame)) { Write-Host "  × 游戏固件烧不进去" -ForegroundColor Red; $okAll = $false }
    else { Ok "  ? 游戏固件烧写成功" }

    $nBoard++
    $dt = ([DateTime]::Now - $t0).TotalSeconds
    $line = "board {0} {1} {2:F0}s" -f $b, $(if ($okAll) { 'OK' } else { 'FAIL' }), $dt
    Add-Content -LiteralPath $log -Value $line -Encoding UTF8
    if ($okAll) { Ok ("===== 第 {0} 块板完成, 用时 {1:F0}s =====" -f $b, $dt) }
    else { Write-Host ("===== 第 {0} 块板失败, 用时 {1:F0}s =====" -f $b, $dt) -ForegroundColor Red }
}

# 收尾: 把 config.h 从"视频烧录模式"恢复成 GAME —— 否则仓库停在烧录模式, 下次谁一 make 就编错固件 ✗
& powershell -ExecutionPolicy Bypass -File $switch game *> $null
Step "已把 config.h 恢复为 GAME 模式(下次跑本脚本会重新切回视频模式, 会多花一次 clean+重编)"

$sw.Stop()
Write-Host ""
Write-Host ("本批完成 {0} 块板, 总用时 {1:N1}s (日志: {2})" -f $nBoard, $sw.Elapsed.TotalSeconds, $log) -ForegroundColor Yellow
if ($Boards -le 1) {
    Write-Host "提示: 设备现在跑游戏固件, 闲置后会待机(SWD 断开); 烧下一块板时若脚本提示'等待 SWD 连接', 点一下屏幕即可。" -ForegroundColor Yellow
}
