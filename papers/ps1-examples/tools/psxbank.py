# Ours. Readers for the inputs of the PS1 example songs:
#   SMF (format 0/1) score, VAB sample bank (pBAV, as written by midi2psm), PSM event stream,
#   and an SPU ADPCM decoder.
import struct

# ---------------------------------------------------------------- SMF
def read_smf(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'MThd'
    fmt, ntrk, ppq = struct.unpack('>HHH', d[8:14])
    pos = 8 + struct.unpack('>I', d[4:8])[0]
    tracks = []
    for _ in range(ntrk):
        assert d[pos:pos + 4] == b'MTrk', d[pos:pos + 4]
        ln = struct.unpack('>I', d[pos + 4:pos + 8])[0]
        t = d[pos + 8:pos + 8 + ln]
        pos += 8 + ln
        evs, i, tick, run = [], 0, 0, 0
        while i < len(t):
            dt = 0
            while True:
                b = t[i]; i += 1
                dt = (dt << 7) | (b & 0x7f)
                if not b & 0x80:
                    break
            tick += dt
            st = t[i]
            if st & 0x80:
                i += 1
            else:
                st = run
            if st == 0xff:
                typ = t[i]; i += 1
                ln2 = 0
                while True:
                    b = t[i]; i += 1
                    ln2 = (ln2 << 7) | (b & 0x7f)
                    if not b & 0x80:
                        break
                evs.append((tick, 'meta', typ, t[i:i + ln2])); i += ln2
            elif st in (0xf0, 0xf7):
                ln2 = 0
                while True:
                    b = t[i]; i += 1
                    ln2 = (ln2 << 7) | (b & 0x7f)
                    if not b & 0x80:
                        break
                i += ln2
            else:
                run = st
                n = 1 if (st & 0xf0) in (0xc0, 0xd0) else 2
                evs.append((tick, st & 0xf0, st & 0x0f, tuple(t[i:i + n]))); i += n
        tracks.append(evs)
    return fmt, ppq, tracks

# ---------------------------------------------------------------- VAB
VAGATR = struct.Struct('<BBBBBbBBBBBBBBBBHHhh8x')

def read_vab(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'pBAV'
    nprog, ntone, nvag = struct.unpack('<HHH', d[18:24])
    mvol, mpan = d[24], d[25]
    progs = []
    for p in range(128):
        o = 32 + p * 16
        progs.append(dict(tones=d[o], mvol=d[o + 1], mpan=d[o + 4]))
    tbase = 32 + 128 * 16
    tones = {}
    for p in range(nprog):
        for k in range(16):
            f = VAGATR.unpack_from(d, tbase + (p * 16 + k) * 32)
            if f[19] < 0 and f[18] == 0 and f[6] == 0 and f[7] == 0:
                continue
            tones[(p, k)] = dict(prior=f[0], mode=f[1], vol=f[2], pan=f[3], center=f[4], shift=f[5],
                                 min=f[6], max=f[7], adsr1=f[16], adsr2=f[17], prog=f[18], vag=f[19])
    obase = tbase + nprog * 16 * 32
    sizes = [x << 3 for x in struct.unpack_from('<256H', d, obase)]
    body = obase + 512
    vags, o = [], body
    for i in range(nvag):
        vags.append(d[o:o + sizes[i]]); o += sizes[i]
    return dict(nprog=nprog, ntone=ntone, nvag=nvag, mvol=mvol, mpan=mpan, progs=progs,
                tones=tones, vags=vags, ram=sum(sizes[:nvag]))

# ---------------------------------------------------------------- SPU ADPCM
FILT = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]

def decode_vag(blob):
    """Decode one sample as the SPU plays it on its first pass.
    Returns (pcm list, loopStart or None, end) in samples; end is where the sample stops
    (one-shot) or the loop end (loop)."""
    out, s1, s2 = [], 0, 0
    loop_start, end = None, None
    nblk = len(blob) // 16
    for b in range(nblk):
        blk = blob[b * 16:(b + 1) * 16]
        shift = blk[0] & 15
        filt = min(blk[0] >> 4, 4)
        flags = blk[1]
        if flags & 4:  # each start flag sets the repeat address: the last one wins
            loop_start = b * 28
        f0, f1 = FILT[filt]
        for i in range(28):
            nib = (blk[2 + i // 2] >> ((i & 1) * 4)) & 15
            if nib >= 8:
                nib -= 16
            sh = 12 - shift
            v = (nib << sh) if sh >= 0 else (nib >> -sh)
            v += (s1 * f0 + s2 * f1 + 32) >> 6
            v = max(-32768, min(32767, v))
            s2, s1 = s1, v
            out.append(v)
        if flags & 1:
            end = (b + 1) * 28
            if not flags & 2:
                loop_start = None  # one-shot: the end flag without repeat stops the voice
            break
    if end is None:
        end = len(out)
    return out, loop_start, end

# ---------------------------------------------------------------- PSM
def read_psm(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'PSM\0'
    ver, rate, n = struct.unpack('<III', d[4:16])
    evs = [struct.unpack_from('<HBBI', d, 16 + i * 8) for i in range(n)]
    return rate / 65536.0, evs
