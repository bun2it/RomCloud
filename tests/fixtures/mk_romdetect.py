#!/usr/bin/env python3
# Dung fixture cho tests/test_romdetect.cpp. Chay lai truoc MOI lan test
# (test co move file). Tao 2 cay tren volume PHAN BIET HOA/THUONG
# (macOS mac dinh khong phan biet -> dung dmg apfs case-sensitive,
#  mount o /tmp/rd2mnt + symlink /tmp/romdetect2 -> .../romdetect2).
import os
import shutil
import struct
import zipfile

DAT = '/Users/tai/RomCloud/assets/fbneosets.dat'
T1 = '/tmp/romdetect/Roms'
T2 = '/tmp/romdetect2/Roms'


def gba_img(tag):
    b = bytearray(256)
    b[4:12] = bytes([0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21])
    b[200:200 + len(tag)] = tag
    return bytes(b)


def make_zip_with_crcs(path, pairs):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, _ in pairs:
            z.writestr(name, b'ROMDATA:' + name.encode() + b'\x00' * 100)
    want = {n.lower(): int(c, 16) for n, c in pairs}
    with open(path, 'r+b') as f:
        data = bytearray(f.read())
    eocd = data.rfind(b'PK\x05\x06')
    assert eocd > 0
    cd_off = struct.unpack('<I', data[eocd + 16:eocd + 20])[0]
    nent = struct.unpack('<H', data[eocd + 10:eocd + 12])[0]
    pos = cd_off
    for _ in range(nent):
        assert data[pos:pos + 4] == b'PK\x01\x02', pos
        fnlen = struct.unpack('<H', data[pos + 28:pos + 30])[0]
        extralen = struct.unpack('<H', data[pos + 30:pos + 32])[0]
        commentsal = struct.unpack('<H', data[pos + 32:pos + 34])[0]
        fname = data[pos + 46:pos + 46 + fnlen].decode('utf-8', 'replace')
        struct.pack_into('<I', data, pos + 16, want[fname.lower()])
        pos += 46 + fnlen + extralen + commentsal
    with open(path, 'wb') as f:
        f.write(data)


def main():
    pairs = []
    for line in open(DAT, encoding='utf-8', errors='replace'):
        p = line.strip().split('|')
        if len(p) >= 3 and p[0].lower() == 'raiden2dx':
            pairs.append((p[1], p[2].lower()))
    assert len(pairs) == 15, len(pairs)

    shutil.rmtree('/tmp/romdetect', ignore_errors=True)
    # T2 nam sau symlink -> volume phan biet hoa/thuong (dmg tren macOS,
    # ext4 tren Linux). Xoa noi dung cu, tao lai thu muc that.
    if os.path.islink('/tmp/romdetect2'):
        target = os.readlink('/tmp/romdetect2')
        shutil.rmtree(target, ignore_errors=True)
        os.makedirs(target, exist_ok=True)
    else:
        shutil.rmtree('/tmp/romdetect2', ignore_errors=True)
    os.makedirs(T1 + '/ARCADE')
    os.makedirs(T1 + '/FC')
    os.makedirs(T1 + '/GBA')
    os.makedirs(T2 + '/gba')
    os.makedirs(T2 + '/GBA')
    os.makedirs(T2 + '/FC')

    make_zip_with_crcs(T1 + '/ARCADE/raiden2dx.zip', pairs)
    shutil.copy(T1 + '/ARCADE/raiden2dx.zip', T1 + '/ARCADE/gamehay.zip')

    A = gba_img(b'A contents................')
    open(T1 + '/GBA/game.gba', 'wb').write(A)
    open(T1 + '/FC/game.gba', 'wb').write(A)
    open(T1 + '/FC/mystery.bin', 'wb').write(b'\xff' * 100)
    open(T1 + '/FC/cover.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + b'\x00' * 32)
    open(T1 + '/FC/game.srm', 'wb').write(b'\x00' * 16)
    open(T1 + '/FC/cart.png', 'wb').write(
        b'pico-8 cartridge // http://www.pico-8.com\n__lua__\nprint(1)')

    B = gba_img(b'B contents................')
    C = gba_img(b'C contents................')
    D = gba_img(b'D contents................')
    assert len({A, B, C, D}) == 4
    open(T2 + '/gba/lower.gba', 'wb').write(B)
    open(T2 + '/GBA/a.gba', 'wb').write(A)
    open(T2 + '/GBA/dup_a.gba', 'wb').write(D)
    open(T2 + '/GBA/dup_b.gba', 'wb').write(D)
    open(T2 + '/GBA/unique.gba', 'wb').write(C)
    open(T2 + '/FC/b.gba', 'wb').write(A)
    open(T2 + '/FC/cover.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + b'\x00' * 32)

    for f in ['/tmp/romdetect_library.db', '/tmp/romdetect_library.db-wal',
              '/tmp/romdetect_library.db-shm', '/tmp/romdetect_settings.json']:
        try:
            os.remove(f)
        except OSError:
            pass
    # verify CRC patch doc duoc
    z = zipfile.ZipFile(T1 + '/ARCADE/gamehay.zip')
    want = {n.lower(): c for n, c in pairs}
    ok = all('%08x' % i.CRC == want[i.filename.lower()]
             for i in z.infolist())
    print('fixture CRC patch:', 'OK' if ok else 'BROKEN')
    print('tree1 + tree2 ready')


if __name__ == '__main__':
    main()
