# 连续烧录视频数据块到外部 Flash(按块序号): 整个 run 只编译一次, 之后每块只打数据补丁
# 用法: powershell -File burn_all.ps1 [-start N] [-count N] [-BinPath 路径] [-ProjPath 工程目录] [-SettleMs N] [-SwdSpeed KHz]
#   -start    起始块号(默认 0)
#   -count    只烧 N 块(默认 0 = 全部), 用于小批量试速
#   -BinPath  指定源视频 .bin(不填则依次在 脚本目录 / 工程内 数据\娜娜莉 / 外层 数据\娜娜莉 找第一个 *.bin)
#   -ProjPath 指定工程目录(不填则从脚本所在目录向上找"含 SPI\SPI_LCD\User\src 的目录")
#   -SettleMs 兜底固定等待(默认 2000ms; 只在固件里查不到进度结构 g_burn_prog 时才会用到)
#   -SwdSpeed SWD 时钟(KHz, 默认 8000=8MHz; 实测 20000 与 8000 无差别, 瓶颈在编译/openocd 启动/固定等待)
#
# 为什么快(★核心): 关键是"不每块重编"
#   ① run 开始: python gen_video_chunk.py <start> 生成 video_chunk.c/.h → make all → 得到"基准镜像";
#      gen 生成的固件把每块参数(偏移/长度/块号/CRC)放在 g_chunk_meta[] 里读, 而不是编译期宏,
#      所以同一份代码可以服务所有块 —— 这是能打补丁的前提。
#   ② 每块: python gen_video_chunk.py <i> raw 只产出 obj/chunk_meta.bin(24B) + obj/chunk_data.bin(53248B),
#      不重编; 然后把这两段按符号地址(段文件偏移 + 段内偏移)贴进基准镜像的副本 obj/burn_patch.elf,
#      烧这个副本(块大小恒定, 不改变 ELF 结构, 只换字节)。
#   ③ 烧完用独立 openocd 会话回读 g_burn_prog, 校验 magic + 块号 + CRC(必须等于 python 算的)
#      才进下一块 —— 补丁贴错位置或用错镜像都能当场发现。
#   实测: 每块 4.0s → ~2.8s; 擦除/写入/校验三段耗时也会打进日志。
param([int]$start = 0, [int]$count = 0, [string]$BinPath = '', [string]$ProjPath = '', [int]$SettleMs = 2000, [int]$SwdSpeed = 8000)

# --- 路径: 脚本可能放在工程内, 也可能放在工程外(原位置 ...\文件数据与源代码\数据\娜娜莉\) ---
# 所以不依赖固定上级目录: 优先 -ProjPath, 否则从脚本所在目录向上找"含 SPI\SPI_LCD\User\src 的目录"。
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

# gen_video_chunk.py: 默认与脚本同目录, 找不到再试工程根
$py = Join-Path $PSScriptRoot "gen_video_chunk.py"
if (-not (Test-Path $py)) { $py = Join-Path $proj "gen_video_chunk.py" }

# 源视频 .bin: -BinPath 优先; 否则 脚本目录 -> 工程内 数据\娜娜莉 -> 外层 数据\娜娜莉
if ($BinPath -ne '') {
    $bin = (Get-Item -LiteralPath $BinPath).FullName          # 外部指定: 直接烧这个文件, 不用搬到本目录
} else {
    $bin = ''
    $outerData = Join-Path (Split-Path (Split-Path $proj -Parent) -Parent) '数据\娜娜莉'
    foreach ($c in @($PSScriptRoot, (Join-Path $proj '数据\娜娜莉'), $outerData)) {
        if ($c -and (Test-Path $c)) {
            $f = Get-ChildItem -Path $c -Filter *.bin | Select-Object -First 1
            if ($f) { $bin = $f.FullName; break }
        }
    }
    if ($bin -eq '') {
        Write-Host "找不到源视频 .bin: 请用 -BinPath <路径> 指定"
        exit 1
    }
}
Write-Host ("视频源: {0}" -f $bin)


$obj      = Join-Path $proj "SPI\SPI_LCD\obj"
$elf      = Join-Path $obj "SPI_LCD.elf"          # 基准镜像(make 产出, 全程只读)
$patchElf = Join-Path $obj "burn_patch.elf"       # 每块现打的镜像副本
$metaBin  = Join-Path $obj "chunk_meta.bin"       # 参数表 6xuint32 = 24 字节
$dataBin  = Join-Path $obj "chunk_data.bin"       # 块数据 53248 字节
$log      = Join-Path $obj "burn_log.txt"
$openocd  = "D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD\bin\openocd.exe"
$cfg      = "D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD\bin\wch-riscv.cfg"
$gcc      = "D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC12\bin"
$mk       = "D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\others\Build_Tools\Make\bin"
$env:Path = "$gcc;$mk;" + $env:Path
$nm       = Join-Path $gcc "riscv-wch-elf-nm.exe"
$objdump  = Join-Path $gcc "riscv-wch-elf-objdump.exe"

# g_burn_prog 结构有效标志(固件里 BURN_PROG_MAGIC = 'B','U','R','N')与字段个数
$BurnMagic = 0x4255524E
$BurnWords = 10

# 查固件里某个全局变量的地址(返回形如 "2000004c"); 查不到返回空串
function Get-SymAddr([string]$name) {
    if (-not (Test-Path $nm)) { return '' }
    $h = (& $nm $elf 2>$null) | Select-String -Pattern ("\s" + $name + "$") | Select-Object -First 1
    if (-not $h) { return '' }
    return ($h.Line.Trim() -split '\s+')[0]
}

# 由变量地址算出它在 ELF 文件里的字节偏移: 先找到它所在的段, 再"段文件偏移 + (地址 - 段VMA)"
function Get-FileOff([string]$vaddrHex) {
    if ($vaddrHex -eq '') { return -1 }
    $va = [Convert]::ToUInt32($vaddrHex, 16)
    foreach ($ln in (& $objdump -h $elf)) {
        if ($ln -match '^\s*\d+\s+\S+\s+([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})\s+[0-9a-fA-F]{8}\s+([0-9a-fA-F]{8})') {
            $sz  = [Convert]::ToUInt32($Matches[1], 16)
            $vma = [Convert]::ToUInt32($Matches[2], 16)
            $off = [Convert]::ToUInt32($Matches[3], 16)
            if ($va -ge $vma -and $va -lt ($vma + $sz)) {
                return [int]($off + ($va - $vma))
            }
        }
    }
    return -1
}

# 把 "0xa,0xb,..." 这 10 个值转成进度对象(独立回读用)
function ConvertFrom-BpHex([string]$list) {
    $v = @()
    foreach ($g in [regex]::Matches($list, '0x[0-9a-fA-F]+')) {
        $v += [Convert]::ToUInt32($g.Value.Substring(2), 16)
    }
    return [pscustomobject]@{
        Magic = $v[0]; Index = $v[1]; Stage = $v[2]; Pct = $v[3]; CrcSrc = $v[4]; CrcRd = $v[5]
        Result = $v[6]; MsErase = $v[7]; MsWrite = $v[8]; MsVerify = $v[9]
    }
}

# 回读固件里的烧录进度结构 g_burn_prog(10 个字), 校验 magic 通过才返回对象; 失败返回 $null
# 每次都用独立的 openocd 会话(约 0.7s): 单次读失败/被打断都能重试, 不像长会话那样一挂全挂。
# ★ magic 校验不能省: MCU 刚复位时读到的可能是编程留在 RAM 里的残留(固定垃圾值), 会冒充"烧完了"。
function Read-BurnProg([string]$addr) {
    $f = Join-Path $obj "burn_prog.txt"
    # 注意: echo 只能给一个参数 —— 多个参数会让 openocd 直接报错退出, 所以用逗号拼成一个
    $a = @('-f', $cfg, '-c', "adapter speed $SwdSpeed", '-c', 'chip_id CH32V20x', '-c', 'init',
           '-c', "mem2array m 32 0x$addr $BurnWords",
           '-c', 'echo BP=$m(0),$m(1),$m(2),$m(3),$m(4),$m(5),$m(6),$m(7),$m(8),$m(9)',
           '-c', 'shutdown')
    & $openocd @a *> $f
    if ($LASTEXITCODE -ne 0) { return $null }
    # 重定向文件按 UTF-8/UTF-16 都可能, 去掉 NUL 保证 ASCII 部分一定能匹配
    $txt = [Text.Encoding]::GetEncoding(936).GetString([IO.File]::ReadAllBytes($f)) -replace "`0", ''
    $hit = [regex]::Match($txt, ('BP=(0x[0-9a-fA-F]+(?:,0x[0-9a-fA-F]+){' + ($BurnWords - 1) + '})'))
    if (-not $hit.Success) { return $null }
    $obj2 = ConvertFrom-BpHex $hit.Groups[1].Value
    if ($obj2.Magic -ne $BurnMagic) { return $null }
    return $obj2
}

Set-Location $obj

$BS = 53248
$blockCount = [int][math]::Ceiling((Get-Item $bin).Length / $BS)
# -count 限制本次只烧 N 块(用于试速); 0 = 烧到文件末尾
$endIndex = $blockCount
if ($count -gt 0 -and ($start + $count) -lt $blockCount) { $endIndex = $start + $count }
Add-Content $log ("==== burn_all(patch) start={0} blocks={1} at {2} ====" -f $start, $blockCount, [DateTime]::Now.ToString('HH:mm:ss'))

# --- 首块准备(同步): 生成 .c/.h + 编译 = 基准镜像; 之后整个 run 不再编译 ---
$po = & python $py $start 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { Add-Content $log ("block {0} GEN FAIL: {1}" -f $start, $po); exit }
$mo = & make all 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { Add-Content $log ("block {0} MAKE FAIL: {1}" -f $start, $mo); exit }
Start-Sleep -Seconds 2

# --- 算出两处补丁在 elf 文件里的偏移(基准镜像后续只读, 每块贴到副本上); 进度结构地址也在这查一次 ---
$metaOff = Get-FileOff (Get-SymAddr 'g_chunk_meta')
$dataOff = Get-FileOff (Get-SymAddr 'g_video_chunk')
$stAddr  = Get-SymAddr 'g_burn_prog'
if ($metaOff -lt 0 -or $dataOff -lt 0) {
    Add-Content $log ("PATCH OFFSET FAIL: meta={0} data={1} (基准镜像里找不到符号)" -f $metaOff, $dataOff)
    exit
}
$base = [IO.File]::ReadAllBytes($elf)
Add-Content $log ("base image: {0} bytes, patch meta@{1} data@{2}" -f $base.Length, $metaOff, $dataOff)

$sw = [System.Diagnostics.Stopwatch]::StartNew()
for ($i = $start; $i -lt $endIndex; $i++) {
    $t0 = $sw.Elapsed.TotalSeconds

    # 1) 只生成这一块的原始数据 + CRC(不重编, 所以很快)
    $po = & python $py $i raw 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        Add-Content $log ("block {0} GEN FAIL: {1}" -f $i, $po); break
    }
    $cm = [regex]::Match($po, 'CRC=0x([0-9a-fA-F]+)')
    if (-not $cm.Success) { Add-Content $log ("block {0} 取不到 CRC: {1}" -f $i, $po); break }
    $expCrc = [Convert]::ToUInt32($cm.Groups[1].Value, 16)

    # 2) 打补丁: 基准镜像副本 + 参数表 24B + 数据 53248B
    $img = New-Object byte[] $base.Length
    [Array]::Copy($base, $img, $base.Length)
    $mb = [IO.File]::ReadAllBytes($metaBin)
    $db = [IO.File]::ReadAllBytes($dataBin)
    [Array]::Copy($mb, 0, $img, $metaOff, $mb.Length)
    [Array]::Copy($db, 0, $img, $dataOff, $db.Length)
    [IO.File]::WriteAllBytes($patchElf, $img)

    # 3) SWD 烧录这份补丁镜像(说明: 试过把"烧录+轮询"塞进同一个 openocd 会话, 但 program 之后
    #    在同一会话里读 RAM 拿到的是固定不变的垃圾值, 只能分开做)
    $oo = & $openocd -f $cfg -c "adapter speed $SwdSpeed" -c "chip_id CH32V20x" -c "program {$patchElf} reset exit" 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        Add-Content $log ("block {0} SWD FAIL: {1}" -f $i, $oo); break
    }

    # 4) 等固件烧完: 结构里的 result 由固件在"写完 + CRC 校验完"之后才置位; 最多等 30s
    #    program 会话自己要 ~2.6s, 结束时固件早就烧完了(复位后约 1.1s), 所以立刻首读即可命中
    $pr   = $null
    $last = $null
    if ($stAddr -ne '') {
        $deadline = (Get-Date).AddSeconds(30)
        while ((Get-Date) -lt $deadline) {
            $r = Read-BurnProg $stAddr        # $null = 回读失败/还是随机 RAM, 直接重试
            if ($r) { $last = $r }
            if ($r -and $r.Result -ne 0) { $pr = $r; break }
            Start-Sleep -Milliseconds 150
        }
        $stgName = @('erase', 'write', 'verify', 'done')
        if (-not $pr) {
            if ($last) {
                Add-Content $log ("block {0} HANDSHAKE TIMEOUT(30s) 停在 stage={1}({2}) pct={3}" -f `
                                  $i, $last.Stage, $stgName[$last.Stage], $last.Pct)
            } else {
                Add-Content $log ("block {0} HANDSHAKE TIMEOUT(30s) 回读一直失败" -f $i)
            }
            break
        } elseif ($pr.Result -ne 1) {
            Add-Content $log ("block {0} BURN FAIL stage={1}({2}) pct={3} crc_src=0x{4:X4} crc_rd=0x{5:X4}" -f `
                              $i, $pr.Stage, $stgName[$pr.Stage], $pr.Pct, $pr.CrcSrc, $pr.CrcRd)
            break
        }
        # ★ 端到端校验: 块号与 CRC 必须和"我以为在烧的块"一致 —— 补丁贴错位置/用错基准镜像都会在这里暴露
        if ($pr.Index -ne $i) {
            Add-Content $log ("block {0} IDX MISMATCH idx={1} (补丁/镜像不对, 已停)" -f $i, $pr.Index); break
        }
        if ($pr.CrcSrc -ne $expCrc) {
            Add-Content $log ("block {0} CRC MISMATCH 固件=0x{1:X4} 期望=0x{2:X4} (补丁贴错位置?, 已停)" -f `
                              $i, $pr.CrcSrc, $expCrc); break
        }
    } else {
        # 固件里查不到进度结构(旧固件): 退回固定等待
        Start-Sleep -Milliseconds $SettleMs
    }

    $tinfo = ''
    if ($pr) {
        $tinfo = (" erase={0}ms write={1}ms verify={2}ms crc=0x{3:X4}" -f `
                  $pr.MsErase, $pr.MsWrite, $pr.MsVerify, $pr.CrcSrc)
    }
    Add-Content $log ("block {0} done offset={1} round={2:N1}s total={3:N1}s{4}" -f `
                      $i, ($i * $BS), ($sw.Elapsed.TotalSeconds - $t0), $sw.Elapsed.TotalSeconds, $tinfo)
}
Add-Content $log ("==== finished total {0:N1}s ====" -f $sw.Elapsed.TotalSeconds)
