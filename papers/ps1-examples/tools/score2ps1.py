#!/usr/bin/env python3
# Ours. Converts a song's original score (SMF, PPQ 48) and the PlayStation sample bank its
# PS1 version plays (VAB) into a plain intermediate that the ps1_examples target turns into
# a .fur:
#   <out>/song.txt   - song/sample/instrument/cell lines (format below)
#   <out>/sNN.raw    - 16-bit little-endian mono PCM per sample, decoded from the bank's ADPCM
#
# The mapping follows the PS1 player (nugget psmplayer) note for note:
#   - engine tick = MIDI tick: rate = PPQ * bpm / 60 Hz. rows are a grid of ticks: a quarter
#     of a beat (PPQ/4 ticks = speed) when every note-on lands on it, else the GCD of the
#     note-on ticks; note-ons never move. a key-off between rows is an FCxx (delayed note
#     release = SPU key-off) on its row, so every key-off stays on its exact tick. a loop end
#     off the grid shortens the last row with 0Fxx (restored by 0Fxx on row 0).
#   - every VAB tone whose key range holds the note sounds (stereo layers = two voices)
#   - voices are allocated like psmplayer (first voice not holding a note), channel = SPU voice
#   - volume = vel^2/127 * CC7 * CC11 * tone vol / 127^3 (0..127), pan = CC10 + tone pan
#   - pitch: the sample's C-4 rate is 44100 * 2^((60 - center)/12) * (1 + shift*38/65536),
#     the player's own fine-tune formula; ADSR words copied from the tone
#   - reverb flag = CC91 > 0 (the player's binary send)
# usage: score2ps1.py <song.mid> <bank.vab> <outdir> <title> [--psm song.psm] [--names song]
#
# song.txt lines:
#   song <hz> <patLen> <rows> <speed> <lastRowSpeed> <rowsPerBeat> <title>
#   sample <id> <c4rate> <loopStart|-1> <loopEnd> <name>
#   ins <id> <sample> <adsr1 hex> <adsr2 hex> <reverb 0/1> <name>
#   cell <ch> <row> <note|off> <ins> <vol> <panL> <panR> <delay>   (panL/panR -1 = unchanged;
#        delay = ticks into the row, key-offs only: 0 = OFF cell, >0 = FCxx effect)
import os
import math
import struct
import sys
from functools import reduce
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psxbank as P  # noqa: E402

GM = {4: 'Electric Piano 1', 9: 'Glockenspiel', 11: 'Vibraphone', 38: 'Synth Bass 1',
      80: 'Square Lead', 89: 'Warm Pad'}
KITB = {0: 'Synth Bass 1', 1: 'Synth Bass 1 (sub)', 2: 'Saw Lead', 3: 'Square Lead',
        4: 'Saw Lead (arp)', 5: 'Square Lead (arp)', 6: 'String Ensemble 1', 7: 'Synth Brass 1 (min)',
        8: 'Synth Brass 1 (maj)', 10: 'Orchestra Hit', 11: 'Reverse Cymbal', 12: 'Saw chord',
        13: 'FX (crash / riser)'}
DRUM = {35: 'Kick 2', 36: 'Kick', 37: 'Side Stick', 38: 'Snare', 39: 'Clap', 40: 'Snare 2',
        41: 'Low Floor Tom', 42: 'Closed Hat', 43: 'High Floor Tom', 44: 'Pedal Hat', 45: 'Low Tom',
        46: 'Open Hat', 47: 'Low-Mid Tom', 48: 'Hi-Mid Tom', 49: 'Crash', 50: 'High Tom',
        51: 'Ride', 52: 'China', 53: 'Ride Bell', 54: 'Tambourine', 55: 'Splash', 56: 'Cowbell',
        57: 'Crash 2', 60: 'Hi Bongo', 62: 'Mute Hi Conga', 64: 'Low Conga', 65: 'High Timbale',
        67: 'High Agogo'}

NOTE_NAMES = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-']


def velcurve(v):
    return (v * v + 63) // 127


def pan_lr(chpan, tonepan):
    eff = max(0, min(127, 64 + (chpan - 64) + (tonepan - 64)))
    l = 127 if eff <= 64 else (127 - eff) * 2
    r = 127 if eff >= 64 else eff * 2
    return min(127, l), min(127, r)


def main():
    a = sys.argv[1:]
    psm = None
    names = 'gm'
    if '--psm' in a:
        i = a.index('--psm'); psm = a[i + 1]; del a[i:i + 2]
    if '--names' in a:
        i = a.index('--names'); names = a[i + 1]; del a[i:i + 2]
    mid, vabp, out, title = a
    os.makedirs(out, exist_ok=True)
    fmt, ppq, trks = P.read_smf(mid)
    bank = P.read_vab(vabp)
    tones = {k: t for k, t in bank['tones'].items()
             if t['vag'] >= 0 and not (t['vol'] == 0 and t['max'] == 0)}

    # ---- score: merged channel events (track order kept for equal ticks), tempo
    ev = []
    tempo = None
    endtick = 0
    for ti, t in enumerate(trks):
        for k, e in enumerate(t):
            endtick = max(endtick, e[0])
            if e[1] == 'meta':
                if e[2] == 0x51:
                    us = int.from_bytes(e[3], 'big')
                    assert tempo is None or tempo == us, 'tempo changes are not handled'
                    tempo = us
                continue
            ev.append((e[0], ti, k, e))
    ev.sort()
    hz = ppq * 1e6 / tempo

    # ---- GM program -> VAB program: midi2psm numbers the bank's programs in order of first
    # use; recover it from the order programs are first heard (drums are the kit program).
    order = []
    prog = [0] * 16
    for tick, _, _, e in ev:
        if e[1] == 0xc0:
            prog[e[2]] = e[3][0]
        elif e[1] == 0x90 and e[3][1] > 0:
            key = ('drum', 0) if e[2] == 9 else ('mel', prog[e[2]])
            if key not in order:
                order.append(key)
    vabprog = {k: i for i, k in enumerate(order)}
    assert len(order) == bank['nprog'], (order, bank['nprog'])

    # ---- note events, per tick: offs first, then ons (by channel, then tone) = psmplayer order
    cc = [dict(vol=100, pan=64, expr=127, rev=40) for _ in range(16)]
    prog = [0] * 16
    pertick = defaultdict(lambda: ([], []))
    held = defaultdict(list)  # (ch,note) -> stack of on ticks, to pair note-offs
    for tick, _, _, e in ev:
        st, ch = e[1], e[2]
        if st == 0xc0:
            prog[ch] = e[3][0]
        elif st == 0xb0:
            num, val = e[3]
            m = {7: 'vol', 10: 'pan', 11: 'expr', 91: 'rev'}
            if num in m:
                assert tick == 0, 'mid-song controller changes are not handled'
                cc[ch][m[num]] = val
        elif st == 0x90 and e[3][1] > 0:
            n, v = e[3]
            vp = vabprog[('drum', 0) if ch == 9 else ('mel', prog[ch])]
            ks = [k for (p, k) in sorted(tones) if p == vp and tones[(p, k)]['min'] <= n <= tones[(p, k)]['max']]
            for k in ks:
                pertick[tick][1].append((ch, n, v, vp, k))
        elif st in (0x80, 0x90):
            pertick[tick][0].append((ch, e[3][0]))

    # ---- optional check against the PSM the PS1 played
    if psm:
        rate, pev = P.read_psm(psm)
        t = 0; pon = Counter(); poff = Counter(); pend = None
        for dt, typ, ch, data in pev:
            t += dt
            if typ == 0:
                pon[(t, ch, data & 255, (data >> 8) & 255, (data >> 16) & 255, data >> 24)] += 1
            elif typ == 1:
                poff[(t, ch, data & 255)] += 1
            elif typ == 12:
                pend = t
        mon = Counter((t, *x) for t, (offs, ons) in pertick.items() for x in ons)
        moff = Counter((t, *x) for t, (offs, ons) in pertick.items() for x in offs)
        print('PSM check: note-ons %d/%d equal=%s, note-offs %d/%d equal=%s, END tick %s' % (
            sum(mon.values()), sum(pon.values()), mon == pon, sum(moff.values()), sum(poff.values()),
            moff == poff, pend))
        assert mon == pon and moff == poff
        endtick = pend

    # ---- samples: one per (vag, center, shift); instruments: one per (sample, adsr, reverb)
    samples = {}
    sinfo = []
    insts = {}
    iinfo = []
    rev_on = [cc[c]['rev'] > 0 for c in range(16)]

    def tone_name(vp, k):
        g = [key for key, i in vabprog.items() if i == vp][0]
        t = tones[(vp, k)]
        if g[0] == 'drum':
            return 'drum %s' % DRUM.get(t['min'], 'key %d' % t['min'])
        nm = (KITB if names == 'kitb' else GM).get(g[1], 'program %d' % g[1])
        lo, hi = t['min'], t['max']
        return '%s %s%d-%s%d' % (nm, NOTE_NAMES[lo % 12], lo // 12 - 1, NOTE_NAMES[hi % 12], hi // 12 - 1)

    def get_ins(vp, k, rev):
        t = tones[(vp, k)]
        skey = (t['vag'], t['center'], t['shift'])
        if skey not in samples:
            pcm, ls, end = P.decode_vag(bank['vags'][t['vag']])
            pcm = pcm[:end]
            c4 = 44100.0 * 2 ** ((60 - t['center']) / 12.0) * (65536 + t['shift'] * 38) / 65536.0
            sid = len(sinfo)
            samples[skey] = sid
            open(os.path.join(out, 's%02d.raw' % sid), 'wb').write(struct.pack('<%dh' % len(pcm), *pcm))
            sinfo.append((sid, int(round(c4)), -1 if ls is None else ls, len(pcm), tone_name(vp, k)))
        sid = samples[skey]
        ikey = (sid, t['adsr1'], t['adsr2'], rev)
        if ikey not in insts:
            insts[ikey] = len(iinfo)
            iinfo.append((len(iinfo), sid, t['adsr1'], t['adsr2'], int(rev), tone_name(vp, k)))
        return insts[ikey]

    # ---- voice allocation (psmplayer: first voice not holding a note; steal never happens here)
    NV = 24
    voice = [None] * NV  # (ch, note) while held
    lastpan = [None] * NV
    cells = []
    maxpoly = 0
    for tick in sorted(pertick):
        offs, ons = pertick[tick]
        offv = set()
        for ch, n in offs:
            for v in range(NV):
                if voice[v] == (ch, n):
                    voice[v] = None
                    offv.add(v)
        onv = set()
        for ch, n, vel, vp, k in sorted(ons, key=lambda x: (x[0], x[4])):
            free = [v for v in range(NV) if voice[v] is None]
            assert free, 'more than 24 voices at tick %d' % tick
            v = free[0]
            voice[v] = (ch, n)
            t = tones[(vp, k)]
            c = cc[ch]
            vol = (velcurve(vel) * c['vol'] * c['expr'] * t['vol']) // (127 * 127 * 127)
            pl, pr = pan_lr(c['pan'], t['pan'])
            pan = (round(pl * 255 / 127), round(pr * 255 / 127))
            ins = get_ins(vp, k, rev_on[ch])
            if pan == lastpan[v]:
                cpan = (-1, -1)
            else:
                cpan = pan
                lastpan[v] = pan
            cells.append((v, tick, n, ins, vol, cpan[0], cpan[1]))
            onv.add(v)
        for v in offv - onv:
            cells.append((v, tick, 'off', -1, -1, -1, -1))
        maxpoly = max(maxpoly, sum(1 for x in voice if x is not None))

    # the PS1 player loops at END: tick END is tick 0 again, so the song is END ticks long.
    # note-offs on the END tick land on tick 0 of the next pass, as they do in the player
    # (a key-on on the same voice at tick 0 wins there too). nothing else sits on END.
    assert all(c[1] < endtick or c[2] == 'off' for c in cells)
    row0 = {c[0] for c in cells if c[1] == 0}
    cells = [c if c[1] < endtick else (c[0], 0) + c[2:] for c in cells
             if c[1] < endtick or c[0] not in row0]
    # ---- row grid: every note-on on a row; key-offs between rows become FCxx
    g_on = reduce(math.gcd, [c[1] for c in cells if c[2] != 'off'])
    grid = ppq // 4 if g_on % (ppq // 4) == 0 else g_on
    rows = -(-endtick // grid)
    lastspeed = endtick - (rows - 1) * grid
    patlen = 4 * ppq // grid  # one 4/4 bar per pattern
    cells = [(c[0], c[1] // grid) + c[2:] + (c[1] % grid,) for c in cells]
    assert all(c[7] == 0 or c[2] == 'off' for c in cells)
    # per voice and row: at most one note-on and one key-off, and a key-off sharing a row
    # with a note-on comes after it (delay > 0), so the two fit in one cell.
    kinds = Counter((c[0], c[1], c[2] == 'off') for c in cells)
    assert max(kinds.values()) == 1
    assert all(not (c[2] == 'off' and c[7] == 0 and (c[0], c[1], False) in kinds) for c in cells)
    with open(os.path.join(out, 'song.txt'), 'w') as f:
        f.write('song %.6f %d %d %d %d %d %s\n' % (hz, patlen, rows, grid, lastspeed, ppq // grid, title))
        for s in sinfo:
            f.write('sample %d %d %d %d %s\n' % s)
        for i in iinfo:
            f.write('ins %d %d %04x %04x %d %s\n' % i)
        for c in sorted(cells, key=lambda x: (x[1], x[0])):
            f.write('cell %d %d %s %d %d %d %d %d\n' % c)
    print('%s: %.4f Hz grid=%d ticks (note-on GCD %d) rows=%d (%.2f s) patLen=%d samples=%d instruments=%d cells=%d max voices=%d '
          'VAB ADPCM=%d B' % (title, hz, grid, g_on, rows, endtick / hz, patlen, len(sinfo), len(iinfo), len(cells),
                              maxpoly, bank['ram']))
    for i in iinfo:
        s = sinfo[i[1]]
        print('  ins %2d  %-34s rate %6d loop %6s..%-6d ADSR %04x/%04x rev %d' % (
            i[0], i[5], s[1], s[2], s[3], i[2], i[3], i[4]))


if __name__ == '__main__':
    main()
