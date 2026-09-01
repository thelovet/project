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
$gcc     = "D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin"
$mk      = "D:\Programs\MounRiver_Studio\MounRiver_Studio\MounRiver_Studio2\resources\app\resources\win32\others\Build_Tools\Make\bin"

$env:Path = "$gcc;$mk;" + $env:Path

# --- 用脚本所在目录自动定位 obj (避免硬编码中文路径) ---
$obj = Join-Path $PSScriptRoot "SPI\SPI_LCD\obj"
$elf = Join-Path $obj "SPI_LCD.elf"
Set-Location $obj

function Do-Build {
    Write-Host "==> make all" -ForegroundColor Cyan
    make all
    if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAIL" -ForegroundColor Red; exit 1 }
    Write-Host "BUILD OK" -ForegroundColor Green
}

function Do-Flash {
    if (-not (Test-Path $elf)) { Write-Host "elf not found: $elf -- build first" -ForegroundColor Red; exit 1 }
    Write-Host "==> SWD program $elf" -ForegroundColor Cyan
    & $openocd -f $cfg -c "adapter speed 8000" -c "chip_id CH32V20x" -c "program {$elf} reset exit"
    if ($LASTEXITCODE -ne 0) { Write-Host "FLASH FAIL" -ForegroundColor Red; exit 1 }
    Write-Host "FLASH OK" -ForegroundColor Green
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
switch ($mode) {
    "build" { Do-Build }
    "flash" { Do-Flash }
    "clean" {
        Write-Host "==> make clean" -ForegroundColor Cyan
        make clean
        Do-Build
        Do-Flash
    }
    default { Do-Build; Do-Flash }
}
$sw.Stop()
Write-Host ("done in {0:N1}s" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Yellow
