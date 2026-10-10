# SOLO-AI eyes  烧录面板 启动脚本
# 用法: 在 Trae 里按本文件的 ▶(运行);  或右键"使用 PowerShell 运行";  或双击同目录的 启动烧录面板.cmd
# 注意: 不要按 F5 调试(会走 PowerShell 扩展的调试通道, 报 ETIMEDOUT); 按 ▶ 即可。
# 设计:
#   1) 服务用 pythonw.exe 起(无控制台窗口), 并且是**本脚本的子进程** ——
#      这样 Trae 的"端口预览"能发现它, 可能自动弹出内置浏览器;
#      本脚本不等它, 起来就退出 → 同一个 ▶ 仍可反复按, 不占运行会话。
#   2) 页面地址: http://127.0.0.1:8767/
# 停止服务: 面板页面上的【停止服务】按钮, 或运行 停止烧录面板.ps1。
$ErrorActionPreference = 'Continue'
Set-Location -LiteralPath $PSScriptRoot

$port = 8767
Write-Host '=================================================='
Write-Host '  SOLO-AI eyes  烧录面板 (字库 / 按钮 / 程序 / 视频)'
Write-Host ("  地址: http://127.0.0.1:{0}/" -f $port)
Write-Host '=================================================='
# 顺手把地址放进剪贴板: 到 Trae 的【浏览器】/【预览】标签的地址栏里 Ctrl+V 即可
try { Set-Clipboard ("http://127.0.0.1:{0}/" -f $port); Write-Host '  (地址已复制到剪贴板: 到 Trae 的【浏览器】/【预览】标签地址栏 Ctrl+V, 或按提示用 Simple Browser 打开)' } catch { }

function Test-Port([int]$p) {
    try { return [bool](Get-NetTCPConnection -LocalPort $p -State Listen -ErrorAction SilentlyContinue) } catch { return $false }
}
if (Test-Port $port) {
    Write-Host '[提示] 服务已经在跑, 页面地址:'
    Write-Host ("  http://127.0.0.1:{0}/" -f $port)
    Write-Host '  停止服务: 页面上的【停止服务】按钮, 或运行 停止烧录面板.ps1'
    exit 0
}

$pyExe = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $pyExe) {
    Write-Host 'PATH 里找不到 python, 无法起服务'
    Read-Host '按回车关闭'
    exit 1
}
# 优先用 pythonw.exe: GUI 子系统程序 → 不会弹控制台窗口
$pyw = Join-Path (Split-Path $pyExe -Parent) 'pythonw.exe'
if (Test-Path -LiteralPath $pyw) { $pyExe = $pyw }

# 起服务(普通子进程; 不等它, 本脚本随即退出)
try {
    $p = Start-Process -FilePath $pyExe -ArgumentList @(('"{0}"' -f $py), "$port") -PassThru -WindowStyle Hidden
} catch {
    Write-Host ('起服务失败: {0}' -f $_.Exception.Message)
    Read-Host '按回车关闭'
    exit 1
}
$srvPid = $p.Id

# 等端口真的开始监听
$ok = $false
for ($i = 0; $i -lt 60; $i++) {
    if (Test-Port $port) { $ok = $true; break }
    Start-Sleep -Milliseconds 300
}
if (-not $ok) {
    Write-Host '服务没能起来 (端口一直没监听)'
    Read-Host '按回车关闭'
    exit 1
}

Write-Host ("  服务已在后台运行 (PID {0}, 无窗口); 本脚本已退出 —— 可以再按 ▶" -f $srvPid)
Write-Host '  打开页面:'
Write-Host ("  http://127.0.0.1:{0}/" -f $port)
Write-Host '    a) Ctrl+Shift+P -> 输入 Simple Browser -> 选 Show -> Ctrl+V(地址已在剪贴板) -> 回车   [内置浏览器]'
Write-Host '    b) 或到 Trae 的【浏览器】/【预览】标签地址栏 Ctrl+V + 回车'
Write-Host  '  停止服务: 面板页面上的【停止服务】按钮, 或运行 停止烧录面板.ps1'
exit 0
