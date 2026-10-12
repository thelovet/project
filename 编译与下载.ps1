# build_flash.ps1 - 一键编译并下载游戏固件到 CH32V203
# 用法:
#   powershell -File build_flash.ps1          # 编译 + 下载 (默认)
#   powershell -File build_flash.ps1 build    # 仅编译
#   powershell -File build_flash.ps1 flash   # 仅下载 (需已编译)
#   powershell -File build_flash.ps1 clean    # 清理后重新编译 + 下载
param([string]$mode = "all")  # all | build | flash | clean

# --- MRS 工具链路径 ---
$openocd = "D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD\bin\openocd.exe"
$cfg     = "D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD\bin\wch-riscv.cfg"
$gcc     = "D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC12\bin"
$mk      = "D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\others\Build_Tools\Make\bin"

$env:Path = "$gcc;$mk;" + $env:Path

# --- 用脚本所在目录自动定位 obj (避免硬编码中文路径) ---
$obj = Join-Path $PSScriptRoot "SPI\SPI_LCD\obj"
$elf = Join-Path $obj "SPI_LCD.elf"
Set-Location $obj

$script:buildOk = $false
$script:flashOk = $false
$script:hadError = 0

function Show-MemUsage {
    $line = (& riscv-wch-elf-size --format=berkeley SPI_LCD.elf 2>$null |
             Where-Object { $_ -match '\.elf$' -and $_ -match '\d' } |
             Select-Object -First 1)
    if (-not $line) { Write-Host "  未能解析内存占用" -ForegroundColor Yellow; return }
    $p = ($line.Trim() -split '\s+')
    $text = [int]$p[0]; $data = [int]$p[1]; $bss = [int]$p[2]
    $flash = $text + $data
    $ram   = $data + $bss
    # 芯片容量: Flash=65280B(0xFF00, 末尾留存档区), RAM=20480B(20K)
    $flashPct = [math]::Round($flash / 65280.0 * 100, 2)
    $ramPct   = [math]::Round($ram / 20480.0 * 100, 2)
    Write-Host ("  Flash 已用: {0} B ({1}% / 65280 B)  剩余: {2} B" -f $flash, $flashPct, (65280 - $flash)) -ForegroundColor Yellow
    Write-Host ("  RAM 已用:   {0} B ({1}% / 20480 B)  剩余: {2} B" -f $ram, $ramPct, (20480 - $ram)) -ForegroundColor Yellow
}

function Do-Build {
    # 先增量编译(快); 若失败(如源码转码后旧.o调试信息失配导致链接错), 再清理过期目标文件重编一次
    Write-Host "==> make all" -ForegroundColor Cyan
    make all
    if ($LASTEXITCODE -ne 0) {
        Write-Host "增量编译失败, 清理过期目标文件后重编..." -ForegroundColor Yellow
        Get-ChildItem -Path $obj -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in ".o",".d",".map",".lst" } | Remove-Item -Force
        make all
        if ($LASTEXITCODE -ne 0) { Write-Host "编译失败" -ForegroundColor Red; exit 1 }
    }
    $script:buildOk = $true
}

function Show-BuildResult {
    Write-Host "编译成功" -ForegroundColor Green
    Show-MemUsage
}

function Do-Flash {
    if (-not (Test-Path $elf)) { Write-Host "未找到 $elf ，请先编译" -ForegroundColor Red; $script:hadError = 1; return }
    Write-Host "==> program $elf" -ForegroundColor Cyan
    & $openocd -f $cfg -c "adapter speed 8000" -c "chip_id CH32V20x" -c "program {$elf} reset exit"
    if ($LASTEXITCODE -ne 0) { $script:flashOk = $false } else { $script:flashOk = $true }
}

function Show-FlashResult {
    if ($script:flashOk) {
        Write-Host "烧录成功" -ForegroundColor Green
    } else {
        Write-Host "烧录失败" -ForegroundColor Red
        Write-Host "  可能原因/处理:" -ForegroundColor Yellow
        Write-Host "  1) 设备待机/没电连不上 -> 先给设备充电, 或点一下屏幕唤醒" -ForegroundColor Yellow
        Write-Host "  2) 连接异常(DM版本错/读保护) -> 断电重启后再试" -ForegroundColor Yellow
        Write-Host "  3) 仍不行 -> 进 BOOT 模式(BOOT0=VCC,BOOT1=GND) 用 ISP 烧录" -ForegroundColor Yellow
        $script:hadError = 1
    }
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
switch ($mode) {
    "build" { Do-Build; Show-BuildResult }
    "flash" { Do-Flash; Show-FlashResult }
    "rebuild" {
        Write-Host "==> 清理" -ForegroundColor Cyan
        make clean
        Do-Build
        Show-BuildResult
    }
    "clean" {
        Write-Host "==> 清理" -ForegroundColor Cyan
        make clean
        Do-Build
        Do-Flash
        Show-BuildResult
        Show-FlashResult
    }
    default { Do-Build; Do-Flash; Show-BuildResult; Show-FlashResult }
}
$sw.Stop()
Write-Host ("完成, 用时 {0:N1}s" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Yellow
if ($script:hadError) { exit 1 }
