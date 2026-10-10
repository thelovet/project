# 停止"烧录面板"的本地服务 (端口 8767)
# 说明: 启动脚本把服务交给独立后台进程了, 所以停它得单独来一步。
#       这里是**故意**按端口找进程 —— 就是要停这个本地服务。
$port = 8767
$c = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue
if ($c) {
    $c | Select-Object -ExpandProperty OwningProcess -Unique | ForEach-Object {
        Write-Host ("停止 PID {0}" -f $_)
        Stop-Process -Id $_ -Force -ErrorAction SilentlyContinue
    }
    # 等端口真正释放: 否则紧接着启动的新进程会绑不上端口(pythonw 无窗口, 会静默退出)
    for ($i = 0; $i -lt 40; $i++) {
        if (-not (Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 250
    }
    Write-Host '烧录面板服务已停止'
} else {
    Write-Host ("端口 {0} 上没有服务在跑" -f $port)
}
