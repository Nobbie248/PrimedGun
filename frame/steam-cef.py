#!/usr/bin/env python3
"""Evaluate JavaScript in Steam's SharedJSContext over the CEF remote debugging port.
Usage: steam-cef.py 'JS expression'   (awaits promises; prints JSON result)

Offline mode, for a copied Steam config (install.sh with PRIMEFRAME_STEAM_DIR; Steam must not be
running when the real file is edited): the same shortcut operations on a shortcuts.vdf.
  steam-cef.py --vdf FILE --name N --exe E --startdir D [--opts O] [--icon I]    add or update; prints the appid
  steam-cef.py --vdf FILE --name N --remove                           remove; prints 1 or 0
"""
import json, os, socket, struct, sys, base64, urllib.request

def ws_connect(url):
    # ws://host:port/path
    rest = url[len('ws://'):]
    hostport, path = rest.split('/', 1)
    host, port = hostport.split(':')
    s = socket.create_connection((host, int(port)), timeout=30)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
    s.sendall(req.encode())
    buf = b''
    while b'\r\n\r\n' not in buf:
        chunk = s.recv(4096)
        if not chunk: raise RuntimeError('handshake failed')
        buf += chunk
    if b' 101 ' not in buf.split(b'\r\n')[0]:
        raise RuntimeError('handshake rejected: ' + buf.decode(errors='replace'))
    return s

def ws_send(s, text):
    data = text.encode()
    hdr = bytearray([0x81])
    n = len(data)
    if n < 126: hdr.append(0x80 | n)
    elif n < 65536: hdr.append(0x80 | 126); hdr += struct.pack('>H', n)
    else: hdr.append(0x80 | 127); hdr += struct.pack('>Q', n)
    mask = os.urandom(4)
    hdr += mask
    s.sendall(bytes(hdr) + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

def recv_exact(s, n):
    buf = b''
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk: raise RuntimeError('connection closed')
        buf += chunk
    return buf

def ws_recv(s):
    while True:
        b1, b2 = recv_exact(s, 2)
        op = b1 & 0x0f
        n = b2 & 0x7f
        if n == 126: n = struct.unpack('>H', recv_exact(s, 2))[0]
        elif n == 127: n = struct.unpack('>Q', recv_exact(s, 8))[0]
        if b2 & 0x80: mask = recv_exact(s, 4)
        else: mask = None
        payload = recv_exact(s, n)
        if mask: payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        if op == 1: return payload.decode()
        if op == 8: raise RuntimeError('closed')
        # ignore ping/pong/binary

def evaluate(expr, title='SharedJSContext'):
    pages = json.load(urllib.request.urlopen('http://127.0.0.1:8080/json', timeout=5))
    page = next(p for p in pages if p.get('title') == title)
    s = ws_connect(page['webSocketDebuggerUrl'])
    ws_send(s, json.dumps({'id': 1, 'method': 'Runtime.evaluate',
                           'params': {'expression': expr, 'awaitPromise': True, 'returnByValue': True}}))
    while True:
        msg = json.loads(ws_recv(s))
        if msg.get('id') == 1:
            s.close()
            if 'error' in msg: raise RuntimeError(msg['error'])
            r = msg['result']
            if 'exceptionDetails' in r:
                raise RuntimeError(json.dumps(r['exceptionDetails'].get('exception', r['exceptionDetails']))[:2000])
            return r['result'].get('value')


# ---- offline shortcuts.vdf editing (binary KeyValues) ----
import zlib

def vdf_parse(data, pos=0):
    out = {}
    while True:
        t = data[pos]; pos += 1
        if t == 8:
            return out, pos
        end = data.index(b'\0', pos); key = data[pos:end].decode(); pos = end + 1
        if t == 0:
            out[key], pos = vdf_parse(data, pos)
        elif t == 1:
            end = data.index(b'\0', pos); out[key] = data[pos:end].decode(errors='replace'); pos = end + 1
        elif t == 2:
            out[key] = struct.unpack('<I', data[pos:pos + 4])[0]; pos += 4
        else:
            raise ValueError('unsupported vdf type %d' % t)

def vdf_dump(d):
    out = b''
    for k, v in d.items():
        kb = k.encode() + b'\0'
        if isinstance(v, dict): out += b'\0' + kb + vdf_dump(v)
        elif isinstance(v, int): out += b'\2' + kb + struct.pack('<I', v & 0xffffffff)
        else: out += b'\1' + kb + str(v).encode() + b'\0'
    return out + b'\x08'

def vdf_load(path):
    try:
        data = open(path, 'rb').read()
    except OSError:
        return {'shortcuts': {}}
    d, _ = vdf_parse(data, 0)
    return d if 'shortcuts' in d else {'shortcuts': {}}

def vdf_save(path, d):
    tmp = path + '.tmp'
    open(tmp, 'wb').write(vdf_dump(d))
    os.replace(tmp, path)

def vdf_shortcut(path, name, exe, startdir, opts, icon=''):
    d = vdf_load(path); sc = d['shortcuts']
    key = next((k for k, v in sc.items() if v.get('AppName') == name), None)
    if key is None:
        key = str(max([int(k) for k in sc] + [-1]) + 1)
    appid = zlib.crc32((exe + name).encode()) | 0x80000000
    old = sc.get(key, {})
    old.update({'appid': appid, 'AppName': name, 'Exe': exe, 'StartDir': startdir, 'LaunchOptions': opts})
    if icon: old['icon'] = icon
    for k, v in (('icon', ''), ('ShortcutPath', ''), ('IsHidden', 0), ('AllowDesktopConfig', 1), ('AllowOverlay', 1),
                 ('OpenVR', 1), ('Devkit', 0), ('DevkitGameID', ''), ('LastPlayTime', 0)):
        old.setdefault(k, v)
    old.setdefault('tags', {})
    sc[key] = old
    vdf_save(path, d)
    return appid

def vdf_remove(path, name):
    d = vdf_load(path); sc = d['shortcuts']
    keys = [k for k, v in sc.items() if v.get('AppName') == name]
    for k in keys: del sc[k]
    if keys: vdf_save(path, d)
    return len(keys)

if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == '--vdf':
        opt = {}
        i = 2
        while i < len(a):
            if a[i] == '--remove': opt['remove'] = True; i += 1
            else: opt[a[i][2:]] = a[i + 1]; i += 2
        if opt.get('remove'):
            print(1 if vdf_remove(a[1], opt['name']) else 0)
        else:
            print(vdf_shortcut(a[1], opt['name'], opt['exe'], opt['startdir'], opt.get('opts', ''), opt.get('icon', '')))
    else:
        print(json.dumps(evaluate(sys.argv[1]), indent=1))
