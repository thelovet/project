# 只编译, 不烧录 (调用 编译与下载.ps1 的 rebuild / build)
# 默认全量编译(make clean + 重编) -> 警告/错误是完整的、真实的
# 用法:
#   powershell -ExecutionPolicy Bypass -File 只编译.ps1            # 全量(默认)
#   powershell -ExecutionPolicy Bypass -File 只编译.ps1 -Fast      # 增量(快, 但看不到旧文件的警告)
#   -Clean 为兼容旧用法, 等同于默认的全量
param([switch]$Clean, [switch]$Fast)

$build = Join-Path $PSScriptRoot '编译与下载.ps1'
if (-not (Test-Path -LiteralPath $build)) {
    Write-Host ("找不到 {0}" -f $build) -ForegroundColor Red
    exit 1
}

# 用 elf 的时间戳判断"这次到底有没有重编"(本工程 make 静默, 日志里没有编译命令行)
$elfPath = Join-Path $PSScriptRoot 'SPI\SPI_LCD\obj\SPI_LCD.elf'
$tBefore = if (Test-Path -LiteralPath $elfPath) { (Get-Item -LiteralPath $elfPath).LastWriteTime } else { Get-Date '2000-01-01' }

$mode = if ($Fast) { 'build' } else { 'rebuild' }
Write-Host ("==> 只编译({0}), 不烧录" -f $(if ($Fast) { '增量' } else { '全量 make clean + 重编' })) -ForegroundColor Cyan

# 边打印边收集, 结束后统计 warning / error
# ★ 必须用 powershell.exe 的绝对路径: 某些环境(如 Trae 内置终端)的 PATH 里没有
#   powershell, 写 "& powershell" 会直接报"无法识别", 而 $LASTEXITCODE 仍保留上一次
#   的值(通常是 0) -> 编译根本没跑, 却打印"编译成功"(误报)。
$psExe = Join-Path $PSHOME 'powershell.exe'
if (-not (Test-Path -LiteralPath $psExe)) {
    Write-Host ("找不到 {0}" -f $psExe) -ForegroundColor Red
    exit 1
}
$log = New-Object System.Collections.ArrayList
$code = 1
try {
    & $psExe -NoProfile -ExecutionPolicy Bypass -File "$build" $mode 2>&1 | ForEach-Object {
        $line = "$_"
        Write-Host $line
        [void]$log.Add($line)
    }
    $code = $LASTEXITCODE
} catch {
    $line = "启动编译失败: " + $_.Exception.Message
    Write-Host $line -ForegroundColor Red
    [void]$log.Add($line)
    $code = 1
}

$tAfter = if (Test-Path -LiteralPath $elfPath) { (Get-Item -LiteralPath $elfPath).LastWriteTime } else { Get-Date '2000-01-01' }
$rebuilt = ($tAfter -ne $tBefore)

# 编译与下载.ps1 有个兜底: 首次编译失败(LTO 偶发 bug)会自动清理目标文件再编一次。
# 统计只算"最终那一次", 否则第一次的报错会被算进来(明明最后成功了却报一堆错误)。
$retried = $false
for ($i = $log.Count - 1; $i -ge 0; $i--) {
    if ($log[$i] -match '清理目标文件后重编') {
        $log = @($log[($i + 1)..($log.Count - 1)])
        $retried = $true
        break
    }
}

$warns = @($log | Where-Object { $_ -match '(?i)\bwarning:' -and $_ -notmatch 'lto-wrapper' })
$errs  = @($log | Where-Object { $_ -match '(?i)\berror:' -or $_ -match '\*\*\*.*Error \d+' })

Write-Host ''
if ($retried) {
    Write-Host '  提示: 首次编译失败(GCC12 LTO 偶发), 已自动清理目标文件重编; 以下只统计最终那次' -ForegroundColor DarkYellow
}
Write-Host ("==== 编译结果: 错误 {0} 个, 警告 {1} 个 ====" -f $errs.Count, $warns.Count) -ForegroundColor $(if ($errs.Count -gt 0) { 'Red' } elseif ($warns.Count -gt 0) { 'Yellow' } else { 'Green' })
if (-not $rebuilt) {
    Write-Host '  注意: 本次没有任何文件被重新编译(elf 未更新) -> 警告数必然为 0; 想看真实结果请去掉 -Fast' -ForegroundColor DarkYellow
}

foreach ($e in ($errs | Select-Object -First 5))  { Write-Host ("  [错误] " + $e.Trim()) -ForegroundColor Red }
foreach ($w in ($warns | Select-Object -First 5)) { Write-Host ("  [警告] " + $w.Trim()) -ForegroundColor Yellow }
if ($errs.Count -gt 5)  { Write-Host ("  ... 另有 {0} 条错误" -f ($errs.Count - 5)) -ForegroundColor Red }
if ($warns.Count -gt 5) { Write-Host ("  ... 另有 {0} 条警告" -f ($warns.Count - 5)) -ForegroundColor Yellow }

if ($code -eq 0) { Write-Host '编译成功(未烧录)' -ForegroundColor Green }
else              { Write-Host ("编译失败, 退出码 {0}" -f $code) -ForegroundColor Red }
exit $code
