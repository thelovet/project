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
IMPORTED_SEGS = os.path.join(CACHE, 'imported_video.segs.json')  # 多文件分段的起止地址
MAXLOG = 3000

# 用绝对路径调 powershell: 服务可能是被 WMI/计划任务拉起的, PATH 里不一定有 powershell
POWERSHELL = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'),
                          'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe')
if not os.path.isfile(POWERSHELL):
    POWERSHELL = 'powershell'


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


def write_video_map(segs):
    """按导入的 bin 段生成固件用的 video_map.h(段数 + 每段起止地址)。"""
    if not PROJ:
        return '没有工程根, 跳过 video_map.h'
    path = os.path.join(PROJ, 'SPI', 'SPI_LCD', 'User', 'inc', 'video_map.h')
    out = []
    out.append('/* 视频段地址表 —— 由烧录面板(导入视频 bin 时)自动生成, 不要手改。')
    out.append('   每段 8 字节 = 起始地址(4 字节, 大端) + 结束地址(4 字节, 大端)。')
    out.append('   段数 = 导入的 bin 个数; 地址按"多个 bin 依序首尾相连烧到 Flash 地址 0"计算。')
    out.append('   生成时间: %s */' % time.strftime('%Y-%m-%d %H:%M:%S'))
    for i, s in enumerate(segs):
        out.append('/* 段%d: %-24s %8d 字节  0x%08X-0x%08X */'
                   % (i + 1, (s.get('name') or '')[:24], s['size'], s['start'], s['end']))
    out.append('#ifndef VIDEO_MAP_H')
    out.append('#define VIDEO_MAP_H')
    out.append('')
    out.append('#define VIDEO_SEGMENT_COUNT  %d' % len(segs))
    out.append('')
    body = []
    for s in segs:
        a, b = s['start'], s['end']
        body.append('    { 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X }'
                    % ((a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255,
                       (b >> 24) & 255, (b >> 16) & 255, (b >> 8) & 255, b & 255))
    out.append('#define VIDEO_RANGES_INIT \\')
    out.append(', \\\n'.join(body))      # 每段之间必须有逗号(行尾续行符)
    out.append('')
    out.append('#endif /* VIDEO_MAP_H */')
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf-8', newline='\n') as fp:
            fp.write('\n'.join(out) + '\n')
        return path
    except OSError as exc:
        return '写 video_map.h 失败: %s' % exc


def imported_info():
    if os.path.isfile(IMPORTED):
        orig = ''
        try:
            with open(IMPORTED_NAME, 'r', encoding='utf-8') as fp:
                orig = fp.read().strip()
        except OSError:
            orig = ''
        segs = []
        try:
            with open(IMPORTED_SEGS, 'r', encoding='utf-8') as fp:
                segs = json.load(fp)
        except (OSError, ValueError):
            segs = []
        size = os.path.getsize(IMPORTED)
        if not segs:
            segs = [{'name': orig, 'size': size, 'start': 0, 'end': size - 1}]
        return {'name': 'imported_video.bin', 'orig': orig, 'size': size, 'segs': segs}
    return None


def runner(step, binpath, with_video):
    cmd = [POWERSHELL, '-ExecutionPolicy', 'Bypass', '-File', BURN_STEP, '-Step', step]
    if binpath:
        cmd += ['-BinPath', binpath]
    if with_video:
        cmd += ['-WithVideo']
    log('执行: ' + ' '.join(cmd))
    ok = False
    try:
        # 子脚本(burn_step/burn_all)里还会用 "& powershell ..." 调下一层, 而本服务可能是
        # WMI/计划任务拉起的, PATH 里没有 PowerShell → 这里补进子进程环境, 一次修到底。
        env = os.environ.copy()
        sysroot = env.get('SystemRoot', r'C:\Windows')
        parts = [p for p in env.get('PATH', '').split(os.pathsep) if p]
        low = [p.lower() for p in parts]
        for extra in (os.path.join(sysroot, 'System32'),
                      os.path.join(sysroot, 'System32', 'WindowsPowerShell', 'v1.0')):
            if extra.lower() not in low:
                parts.append(extra)
        env['PATH'] = os.pathsep.join(parts)

        # CREATE_NO_WINDOW: 不给子进程建可见控制台, 它下面的 make/openocd 也继承这个不可见控制台 → 全程不弹窗口
        p = subprocess.Popen(cmd, cwd=HERE, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, bufsize=0, env=env,
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
            # 多文件: ?segs=[{"name":..,"size":..}, ...] —— 按选择顺序首尾相连烧到地址 0
            segs = []
            try:
                q = urllib.parse.parse_qs(self.path.split('?', 1)[1]) if '?' in self.path else {}
                raw_items = json.loads((q.get('segs') or [''])[0] or '[]')
                off = 0
                for it in raw_items:
                    sz = int(it.get('size') or 0)
                    if sz <= 0:
                        continue
                    segs.append({'name': str(it.get('name') or ''), 'size': sz,
                                 'start': off, 'end': off + sz - 1})
                    off += sz
                if off != len(data):
                    segs = []          # 大小对不上: 退回单段
            except (ValueError, TypeError):
                segs = []
            if not segs:
                segs = [{'name': orig, 'size': len(data), 'start': 0, 'end': len(data) - 1}]
            try:
                with open(IMPORTED_SEGS, 'w', encoding='utf-8') as fp:
                    json.dump(segs, fp, ensure_ascii=False)
            except OSError:
                pass
            mp = write_video_map(segs)
            log('video_map.h -> %s' % mp)
            md5 = hashlib.md5(data).hexdigest().upper()
            log('导入视频 bin: %d 字节, %d 段, MD5=%s' % (len(data), len(segs), md5))
            return self._json({'ok': True, 'size': len(data), 'md5': md5, 'orig': orig, 'segs': segs})

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
