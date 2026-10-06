#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_hap.py -- 打包产物(未签名 .hap)自检

为什么要有这个: App 里那一套 verify_*.py 检查的是**源码**, 但用户拿到手的是 .hap。
这两者之间隔着资源编译 / 打包 / strip 三道工序, 任何一道出岔子, 源码全绿也白搭
(真出过: 图标一直是旧的那一张, 直到把 hap 拆开才看见)。

用法:
    python verify_hap.py <hap 路径> [仓库根目录]

退出码 0 = 全部通过; 1 = 有硬缺陷(逐条打印)。
"""
import hashlib
import json
import os
import re
import struct
import sys
import zipfile

FAIL = []
PASS = []


def ck(name, cond, detail=''):
    (PASS if cond else FAIL).append(name + (('  -- ' + detail) if detail else ''))


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def elf_sections(blob):
    """返回 ELF64 .so 的节名集合(节名字符串直接扫 .shstrtab 也行, 这里做正规解析)。"""
    if len(blob) < 64 or blob[:4] != b'\x7fELF':
        return set()
    is64 = blob[4] == 2
    if not is64:
        return set()
    e_shoff = struct.unpack_from('<Q', blob, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', blob, 0x3A)[0]
    e_shnum = struct.unpack_from('<H', blob, 0x3C)[0]
    e_shstrndx = struct.unpack_from('<H', blob, 0x3E)[0]
    if e_shoff == 0 or e_shnum == 0 or e_shstrndx >= e_shnum:
        return set()
    base = e_shoff + e_shstrndx * e_shentsize
    if base + 24 > len(blob):
        return set()
    str_off = struct.unpack_from('<Q', blob, base + 0x18)[0]
    str_size = struct.unpack_from('<Q', blob, base + 0x20)[0]
    if str_off + str_size > len(blob):
        return set()
    shstr = blob[str_off:str_off + str_size]
    names = set()
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if off + 4 > len(blob):
            break
        n = struct.unpack_from('<I', blob, off)[0]
        end = shstr.find(b'\x00', n)
        if 0 <= n < len(shstr):
            names.add(shstr[n:end if end >= 0 else len(shstr)].decode('ascii', 'replace'))
    return names


def main():
    if len(sys.argv) < 2:
        print('usage: verify_hap.py <hap> [repo]')
        return 2
    hap = os.path.abspath(sys.argv[1])
    repo = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.dirname(os.path.dirname(hap))

    ck('hap 存在', os.path.exists(hap), hap)
    if not os.path.exists(hap):
        return 1
    size = os.path.getsize(hap)
    # 下限挡"打空了"; 上限挡"strip 关掉之后又变回带符号的大包"这种意外
    ck('hap 体积合理(>8MB)', size > 8 * 1024 * 1024, str(size) + ' B')

    z = zipfile.ZipFile(hap)
    names = set(z.namelist())

    for must in ['module.json', 'pack.info', 'ets/modules.abc', 'resources.index',
                 'resources/base/profile/main_pages.json']:
        ck('包内含 ' + must, must in names)

    libs = sorted(n for n in names if n.startswith('libs/arm64-v8a/') and n.endswith('.so'))
    ck('libs/arm64-v8a 有 6 个 .so', len(libs) == 6, ', '.join(os.path.basename(x) for x in libs))
    ck('没有别的 ABI 目录', not any(n.startswith('libs/') and not n.startswith('libs/arm64-v8a/') for n in names))

    # ---- module.json ----
    mj = json.loads(z.read('module.json').decode('utf-8'))
    app = mj.get('app', {})
    mod = mj.get('module', {})
    ck('bundleName 未变', app.get('bundleName') == 'com.aurora.bench', str(app.get('bundleName')))
    label = app.get('label')
    ck('app.label 走资源引用', label == '$string:app_name', str(label))
    ck('三种设备形态齐全',
       set(mod.get('deviceTypes', [])) >= {'default', 'tablet', '2in1'},
       str(mod.get('deviceTypes')))
    ck('module 是 entry 且非免安装',
       mod.get('type') == 'entry' and mod.get('installationFree') is False)
    # minAPIVersion 不再写死: 从 build-profile.json5 的 compatibleSdkVersion "6.0.0(20)" 推导
    # 编码规则 = 主版本 *1e7 + 次版本 *1e5 + 修订 *1e3 + API 号
    #   (5.1.0(18) -> 50100018, 6.0.0(20) -> 60000020 —— 都拿真机 hap 里的 module.json 核对过)
    # 别写成 *1e6/*1e4/*1e2: 那样 6.0.0(20) 会算成 6000020, 少一位。
    want_min = None
    bpp = os.path.join(repo, 'build-profile.json5')
    if os.path.exists(bpp):
        m = re.search(r'"compatibleSdkVersion"\s*:\s*"([0-9]+)\.([0-9]+)\.([0-9]+)\(([0-9]+)\)"',
                      open(bpp, encoding='utf-8').read())
        if m:
            want_min = (int(m.group(1)) * 10000000 + int(m.group(2)) * 100000 +
                        int(m.group(3)) * 1000 + int(m.group(4)))
    ck('minAPIVersion 与 build-profile 的 compatibleSdkVersion 对得上',
       want_min is not None and app.get('minAPIVersion') == want_min,
       'hap=' + str(app.get('minAPIVersion')) + ' expect=' + str(want_min))

    # ---- 版本号必须与 AppScope/app.json5 一致 ----
    ap = os.path.join(repo, 'AppScope', 'app.json5')
    if os.path.exists(ap):
        raw = open(ap, encoding='utf-8').read()
        vn = re.search(r'"versionName"\s*:\s*"([^"]+)"', raw)
        vc = re.search(r'"versionCode"\s*:\s*(\d+)', raw)
        ck('versionName 与 app.json5 一致',
           vn and app.get('versionName') == vn.group(1),
           'hap=' + str(app.get('versionName')) + ' src=' + (vn.group(1) if vn else '?'))
        ck('versionCode 与 app.json5 一致',
           vc and app.get('versionCode') == int(vc.group(1)),
           'hap=' + str(app.get('versionCode')) + ' src=' + (vc.group(1) if vc else '?'))
    else:
        ck('能找到 AppScope/app.json5', False, ap)

    # ---- 仓库里四个图标文件必须两两一致(AppScope 与 entry 各一份 app_icon / startIcon) ----
    icons = ['AppScope/resources/base/media/app_icon.png',
             'entry/src/main/resources/base/media/app_icon.png',
             'AppScope/resources/base/media/startIcon.png',
             'entry/src/main/resources/base/media/startIcon.png']
    digests = {}
    for rel in icons:
        p = os.path.join(repo, rel)
        ck('仓库里有 ' + rel, os.path.exists(p))
        if os.path.exists(p):
            digests[rel] = sha256(open(p, 'rb').read())
    ck('两个 app_icon.png 逐字节相同',
       digests.get(icons[0]) == digests.get(icons[1]),
       str(digests.get(icons[0], '?'))[:12] + ' vs ' + str(digests.get(icons[1], '?'))[:12])
    ck('两个 startIcon.png 逐字节相同',
       digests.get(icons[2]) == digests.get(icons[3]),
       str(digests.get(icons[2], '?'))[:12] + ' vs ' + str(digests.get(icons[3], '?'))[:12])

    # ---- 图标: 包里的 app_icon 必须与仓库里那张逐字节相同 ----
    for rel, key in [('AppScope/resources/base/media/app_icon.png', 'resources/base/media/app_icon.png'),
                     ('AppScope/resources/base/media/startIcon.png', 'resources/base/media/startIcon.png')]:
        sp = os.path.join(repo, rel)
        ck('仓库里有 ' + rel, os.path.exists(sp))
        if os.path.exists(sp) and key in names:
            a = sha256(open(sp, 'rb').read())
            b = sha256(z.read(key))
            ck('包内 ' + os.path.basename(key) + ' 与仓库同源', a == b, a[:12] + ' vs ' + b[:12])
            blob = z.read(key)
            w, h = struct.unpack('>II', blob[16:24])
            ck(os.path.basename(key) + ' 是 512x512',
               (w, h) == (512, 512), str(w) + 'x' + str(h))

    # ---- 资源索引里必须能找到新的 App 名 (字符串被编进 resources.index) ----
    if 'resources.index' in names:
        idx = z.read('resources.index')
        try:
            want = json.load(open(os.path.join(repo, 'AppScope/resources/base/element/string.json'),
                                  encoding='utf-8'))['string'][0]['value']
        except Exception:
            want = None
        if want:
            ck('resources.index 里有 App 名 ' + want, want.encode('utf-8') in idx)

    # ---- 原生库: 不许被 strip (strip 掉就还原不出崩溃函数名) ----
    for n in libs:
        base = os.path.basename(n)
        blob = z.read(n)
        secs = elf_sections(blob)
        if not secs:
            ck('能解析 ' + base + ' 的节表', False)
            continue
        if base == 'libc++_shared.so':
            # SDK 自带的预编译运行时, 一直是 strip 过的; 我们也不靠它还原函数名
            ck('libc++_shared.so 存在且非空', len(blob) > 500000, str(len(blob)) + ' B')
            continue
        ck(base + ' 保留符号表(.symtab)', '.symtab' in secs,
           ','.join(sorted(s for s in secs if s in ('.symtab', '.debug_info', '.debug_line'))))
        # 大小不设绝对阈值(gpu / gpu7 本来就小), 用"调试节还在不在"判有没有被 strip
        ck(base + ' 没被 strip(.debug_info 还在)', '.debug_info' in secs, str(len(blob)) + ' B')

    # ---- ArkTS 产物 ----
    if 'ets/modules.abc' in names:
        ck('modules.abc 非空', len(z.read('ets/modules.abc')) > 200000,
           str(len(z.read('ets/modules.abc'))) + ' B')

    print('hap   : ' + hap)
    print('size  : ' + str(size) + ' B')
    print('sha256: ' + sha256(open(hap, 'rb').read()))
    print('')
    for p in PASS:
        print('  OK   ' + p)
    for f in FAIL:
        print('  FAIL ' + f)
    print('')
    print('VERIFY_HAP ok=%d fail=%d' % (len(PASS), len(FAIL)))
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
