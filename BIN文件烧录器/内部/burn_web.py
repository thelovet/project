#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""烧录面板本地服务 (只监听 127.0.0.1)。

为什么需要它: 浏览器页面不能直接调 openocd / make / PowerShell, 所以由本服务
在后台跑 burn_step.ps1, 并把日志回传给页面。页面(外部Flash存储分布图.html)
本身也能单独打开看存储分布图, 只是上面的烧录按钮不可用。

用法:  python burn_web.py [端口]      默认端口 8767
       或双击 启动烧录面板.ps1 (会起服务并自动打开浏览器)
"""
import hashlib
import http.server
import urllib.parse
import json
import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8767
PAGE = '外部Flash存储分布图.html'
CACHE = os.path.join(HERE, 'cache')
BURN_STEP = os.path.join(HERE, 'burn_step.ps1')
IMPORTED = os.path.join(CACHE, 'imported_video.bin')
IMPORTED_NAME = os.path.join(CACHE, 'imported_video.name')   # 导入时的原文件名
MAXLOG = 3000


def find_proj(start):
    d = start
    for _ in range(8):
        if os.path.isdir(os.path.join(d, 'SPI', 'SPI_LCD', 'User', 'src')):
            return d
        nd = os.path.dirname(d)
        if nd == d:
            break
        d = nd
    return None


PROJ = find_proj(HERE)

_server = None
_lock = threading.Lock()
_state = {
    'busy': False,
    'step': '',
    'started': 0,
    'finished': 0,
    'result': '',      # '' = 还没跑过, OK / FAIL
    'log': [],
}


def log(msg):
    _state['log'].append(msg)
    if len(_state['log']) > MAXLOG:
        del _state['log'][0:len(_state['log']) - MAXLOG]


def imported_info():
    if os.path.isfile(IMPORTED):
        orig = ''
        try:
            with open(IMPORTED_NAME, 'r', encoding='utf-8') as fp:
                orig = fp.read().strip()
        except OSError:
            orig = ''
        return {'name': 'imported_video.bin', 'orig': orig, 'size': os.path.getsize(IMPORTED)}
    return None


def runner(step, binpath, with_video):
    cmd = ['powershell', '-ExecutionPolicy', 'Bypass', '-File', BURN_STEP, '-Step', step]
    if binpath:
        cmd += ['-BinPath', binpath]
    if with_video:
        cmd += ['-WithVideo']
    log('执行: ' + ' '.join(cmd))
    ok = False
    try:
        # CREATE_NO_WINDOW: 不给子进程建可见控制台, 它下面的 make/openocd 也继承这个不可见控制台 → 全程不弹窗口
        p = subprocess.Popen(cmd, cwd=HERE, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, bufsize=0,
                             creationflags=0x08000000)
        for raw in iter(p.stdout.readline, b''):
            line = raw.decode('gbk', errors='replace').rstrip('\r\n')
            if line.strip():
                log(line)
        p.wait()
        ok = (p.returncode == 0)
    except Exception as exc:                       # noqa: BLE001
        log('运行异常: %r' % (exc,))
    with _lock:
        _state['busy'] = False
        _state['finished'] = time.time()
        _state['result'] = 'OK' if ok else 'FAIL'
    log('===== 结束: %s =====' % ('成功' if ok else '失败'))


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=HERE, **kw)

    def log_message(self, fmt, *args):
        pass                                       # 静音访问日志

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split('?')[0]
        if path in ('/', '/index.html'):
            self.path = '/' + PAGE
            return http.server.SimpleHTTPRequestHandler.do_GET(self)
        if path == '/api/state':
            with _lock:
                st = dict(_state)
                st['log'] = list(_state['log'])
            st['bin'] = imported_info()
            st['proj'] = PROJ
            st['proj_ok'] = PROJ is not None and os.path.isfile(BURN_STEP)
            return self._json(st)
        return http.server.SimpleHTTPRequestHandler.do_GET(self)

    def do_POST(self):
        path = self.path.split('?')[0]
        n = int(self.headers.get('Content-Length') or 0)

        if path == '/api/quit':
            self._json({'ok': True, 'msg': '服务正在停止'})
            def _stop():
                time.sleep(0.3)
                if _server is not None:
                    _server.shutdown()
            threading.Thread(target=_stop, daemon=True).start()
            return

        if path == '/api/import_bin':
            data = self.rfile.read(n) if n else b''
            if not data:
                return self._json({'ok': False, 'msg': '没有收到数据'}, 400)
            os.makedirs(CACHE, exist_ok=True)
            with open(IMPORTED, 'wb') as f:
                f.write(data)
            orig = ''
            if '?' in self.path:
                orig = (urllib.parse.parse_qs(self.path.split('?', 1)[1]).get('name') or [''])[0]
            try:
                with open(IMPORTED_NAME, 'w', encoding='utf-8') as fp:
                    fp.write(orig)
            except OSError:
                pass
            md5 = hashlib.md5(data).hexdigest().upper()
            log('导入视频 bin: %d 字节, MD5=%s%s' % (len(data), md5, (', 原文件名=' + orig) if orig else ''))
            return self._json({'ok': True, 'size': len(data), 'md5': md5, 'orig': orig})

        if path == '/api/run':
            body = self.rfile.read(n) if n else b'{}'
            try:
                req = json.loads(body.decode('utf-8') or '{}')
            except Exception:                      # noqa: BLE001
                req = {}
            with _lock:
                if _state['busy']:
                    return self._json({'ok': False, 'msg': '已有烧录任务在跑'}, 409)
                step = str(req.get('step') or 'auto')
                _state['busy'] = True
                _state['step'] = step
                _state['started'] = time.time()
                _state['finished'] = 0
                _state['result'] = ''
                _state['log'] = []
            use_bin = bool(req.get('useBin')) and os.path.isfile(IMPORTED)
            t = threading.Thread(target=runner,
                                 args=(step, IMPORTED if use_bin else '', bool(req.get('withVideo'))),
                                 daemon=True)
            t.start()
            return self._json({'ok': True})

        return self._json({'ok': False, 'msg': '未知接口'}, 404)


def port_listening(port):
    s = socket.socket()
    s.settimeout(0.4)
    try:
        s.connect(('127.0.0.1', port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def main():
    global _server
    if PROJ is None:
        print('警告: 没找到工程根(应含 SPI\\SPI_LCD\\User\\src), 烧录会失败')
    if not os.path.isfile(BURN_STEP):
        print('警告: 找不到 burn_step.ps1')
    os.makedirs(CACHE, exist_ok=True)
    if port_listening(PORT):
        print('端口 %d 已被占用(服务可能已经在跑)' % PORT)
        print('直接打开 http://127.0.0.1:%d/ 即可' % PORT)
        return
    try:
        srv = http.server.ThreadingHTTPServer(('127.0.0.1', PORT), Handler)
        _server = srv
    except OSError as exc:
        print('端口 %d 已被占用(服务可能已经在跑): %s' % (PORT, exc))
        print('直接打开 http://127.0.0.1:%d/ 即可' % PORT)
        return
    print('烧录面板: http://127.0.0.1:%d/' % PORT)
    print('工程根  : %s' % PROJ)
    print('按 Ctrl+C 停止服务')
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    srv.server_close()


if __name__ == '__main__':
    main()
