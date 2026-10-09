#!/usr/bin/env python3
# Test data for codectest and blocktest: python3 make_vectors.py outdir eboot.prx
# Needs Pillow, zstandard, genisoimage, chdman and zip.
import gzip, os, random, struct, subprocess, sys, zipfile, zlib
import zstandard
from PIL import Image

out, prx = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
rnd = random.Random(7)

# Raw blobs in every zlib-family wrapper and several zstd levels.
os.makedirs(os.path.join(out, 'in'), exist_ok=True)
blobs = {
    'text': b''.join(rnd.choice([b'psp ', b'umd ', b'sector ', b'\n', b'zlib ']) for _ in range(200000)),
    'rand': rnd.randbytes(300000),
    'zero': bytes(1 << 20),
    'small': b'hello',
    'empty': b'',
}
for k, v in blobs.items():
    base = os.path.join(out, 'in', k)
    open(base + '.raw', 'wb').write(v)
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    open(base + '.deflate', 'wb').write(c.compress(v) + c.flush())
    c = zlib.compressobj(6, zlib.DEFLATED, -15, 8, zlib.Z_FIXED)
    open(base + '.fixed', 'wb').write(c.compress(v) + c.flush())
    open(base + '.zlib', 'wb').write(zlib.compress(v, 6))
    open(base + '.gz', 'wb').write(gzip.compress(v, 9))
    for lvl in (1, 3, 9, 19):
        cctx = zstandard.ZstdCompressor(level=lvl, write_checksum=True, write_content_size=True)
        open(base + '.l%d.zst' % lvl, 'wb').write(cctx.compress(v))
    open(base + '.adler', 'w').write('%08x' % zlib.adler32(v))
    open(base + '.crc', 'w').write('%08x' % zlib.crc32(v))

# PNGs in each colour type, with the RGBA they should decode to.
pngdir = os.path.join(out, 'png')
os.makedirs(pngdir, exist_ok=True)
img = Image.new('RGBA', (61, 47))
img.putdata([((x * 4) & 255, (y * 5) & 255, (x * y) & 255, (x + y * 3) & 255) for y in range(47) for x in range(61)])
img.save(os.path.join(pngdir, 'rgba.png'))
img.convert('RGB').save(os.path.join(pngdir, 'rgb.png'))
img.convert('L').save(os.path.join(pngdir, 'gray.png'))
img.convert('LA').save(os.path.join(pngdir, 'graya.png'))
img.convert('P', palette=Image.ADAPTIVE).save(os.path.join(pngdir, 'pal.png'))
img.convert('P', palette=Image.ADAPTIVE).save(os.path.join(pngdir, 'paltrns.png'), transparency=0)
Image.new('RGB', (300, 200), (10, 200, 30)).save(os.path.join(pngdir, 'solid.png'), optimize=True)
for n in os.listdir(pngdir):
    if n.endswith('.png'):
        open(os.path.join(pngdir, n + '.rgba'), 'wb').write(Image.open(os.path.join(pngdir, n)).convert('RGBA').tobytes())
# 16 bits per sample decodes to the high byte.
g16 = [(i * 97) % 65536 for i in range(40 * 30)]
Image.frombytes('I;16', (40, 30), struct.pack('<%dH' % len(g16), *g16)).save(os.path.join(pngdir, 'gray16.png'))
open(os.path.join(pngdir, 'gray16.png.rgba'), 'wb').write(b''.join(bytes((v >> 8, v >> 8, v >> 8, 255)) for v in g16))

# A zip for the VFS reader: directories, stored and deflated members.
with zipfile.ZipFile(os.path.join(out, 'vfs.zip'), 'w') as z:
    z.writestr('textures.ini', b'[hashes]\n')
    z.writestr('dir/', b'')
    z.writestr(zipfile.ZipInfo('dir/Stored.bin'), blobs['rand'][:5000], compress_type=zipfile.ZIP_STORED)
    z.writestr('dir/sub/deflated.txt', blobs['text'], compress_type=zipfile.ZIP_DEFLATED)
    z.writestr('empty.txt', b'')

# A bootable disc image, and the same image as CSO, CHD and inside zips.
root = os.path.join(out, 'isoroot')
os.makedirs(os.path.join(root, 'PSP_GAME', 'SYSDIR'), exist_ok=True)
os.makedirs(os.path.join(root, 'DATA'), exist_ok=True)
open(os.path.join(root, 'PSP_GAME', 'SYSDIR', 'EBOOT.BIN'), 'wb').write(open(prx, 'rb').read())

def sfo(entries):
    keys = b''; data = b''; idx = b''
    for k, v in entries:
        if isinstance(v, int):
            raw = struct.pack('<I', v); fmt = 0x0404; ln = mx = 4
        else:
            raw = v.encode() + b'\0'; fmt = 0x0204; ln = len(raw); mx = (ln + 3) & ~3
            raw = raw.ljust(mx, b'\0')
        idx += struct.pack('<HHIII', len(keys), fmt, ln, mx, len(data))
        keys += k.encode() + b'\0'; data += raw
    while len(keys) % 4:
        keys += b'\0'
    kt = 20 + len(idx)
    return struct.pack('<4sIIII', b'\0PSF', 0x101, kt, kt + len(keys), len(entries)) + idx + keys + data

open(os.path.join(root, 'PSP_GAME', 'PARAM.SFO'), 'wb').write(sfo([
    ('CATEGORY', 'UG'), ('DISC_ID', 'TEST00001'), ('DISC_VERSION', '1.00'),
    ('PARENTAL_LEVEL', 1), ('PSP_SYSTEM_VER', '1.00'), ('REGION', 32768), ('TITLE', 'Codec test')]))
open(os.path.join(root, 'DATA', 'random.bin'), 'wb').write(rnd.randbytes(8 << 20))
open(os.path.join(root, 'DATA', 'text.bin'), 'wb').write(blobs['text'] * 20)
open(os.path.join(root, 'DATA', 'zero.bin'), 'wb').write(bytes(4 << 20))
iso = os.path.join(out, 'test.iso')
subprocess.check_call(['genisoimage', '-quiet', '-iso-level', '4', '-sysid', 'PSP GAME', '-V', 'TEST', '-o', iso, root])

def cso(src, dst, block):
    data = open(src, 'rb').read()
    n = (len(data) + block - 1) // block
    hdr = struct.pack('<4sIQIBB2s', b'CISO', 0x18, len(data), block, 1, 0, b'\0\0')
    body = bytearray(); index = []
    pos = len(hdr) + 4 * (n + 1)
    for i in range(n):
        chunk = data[i * block:(i + 1) * block].ljust(block, b'\0')
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        comp = c.compress(chunk) + c.flush()
        if len(comp) >= block:
            index.append((pos + len(body)) | 0x80000000); body += chunk
        else:
            index.append(pos + len(body)); body += comp
    index.append(pos + len(body))
    with open(dst, 'wb') as f:
        f.write(hdr); f.write(struct.pack('<%dI' % len(index), *index)); f.write(body)

cso(iso, os.path.join(out, 'test.cso'), 2048)
cso(iso, os.path.join(out, 'test16k.cso'), 16384)
for name, extra in [('test.chd', []), ('test_zstd.chd', ['-c', 'zstd']), ('test_zlib.chd', ['-c', 'zlib']),
                    ('test_lzma.chd', ['-c', 'lzma']), ('test_huff.chd', ['-c', 'huff']),
                    ('test_big.chd', ['-hs', '65536', '-c', 'zstd,lzma'])]:
    subprocess.check_call(['chdman', 'createdvd', '-f', '-i', iso, '-o', os.path.join(out, name)] + extra,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for name, level in [('stored.zip', '-0'), ('deflated.zip', '-9')]:
    path = os.path.join(out, name)
    if os.path.exists(path):
        os.remove(path)
    subprocess.check_call(['zip', '-q', level, '-j', path, iso])
