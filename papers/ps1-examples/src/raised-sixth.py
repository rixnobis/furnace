#!/usr/bin/env python3
# "Raised Sixth" - a waltz in D Dorian with a trio in B-flat Lydian. Writes an SMF (PPQ 48)
# whose programs are the Forty-One Cycles bank's (kitb numbering), for score2ps1.py.
# usage: raised-sixth.py <out.mid>
import struct
import sys

PPQ = 48
BPM = 144
BAR = 3 * PPQ

# kitb programs (score2ps1 --names kitb): 0 Synth Bass 1, 2 Saw Lead, 3 Square Lead,
# 5 Square Lead (arp), 6 String Ensemble 1
CH_BASS, CH_CHORD, CH_MEL, CH_CTR, CH_STR = 0, 1, 2, 3, 4
SETUP = {  # ch: (program, CC7, CC10 pan, CC91 reverb send)
    CH_BASS: (0, 116, 64, 0),
    CH_CHORD: (5, 74, 50, 30),
    CH_MEL: (2, 98, 76, 50),
    CH_CTR: (3, 70, 40, 50),
    CH_STR: (6, 112, 84, 60),
}

PC = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}


def midi(n):
    """'D5', 'Bb4', 'F#3' -> MIDI number (C4 = 60)."""
    p = PC[n[0]]
    i = 1
    while n[i] in 'b#':
        p += -1 if n[i] == 'b' else 1
        i += 1
    return p + 12 * (int(n[i:]) + 1)


def chord(name):
    """'Dm', 'G', 'C/Bb', 'Em', 'Dm6' -> (bass pc, [pcs], root pc)"""
    bass = None
    if '/' in name:
        name, b = name.split('/')
        bass = midi(b + '0') % 12
    r = PC[name[0]]
    q = name[1:]
    if q.startswith('b'):
        r -= 1
        q = q[1:]
    r %= 12
    if q == 'm':
        pcs = [r, r + 3, r + 7]
    elif q == 'm6':
        pcs = [r, r + 3, r + 7, r + 9]
    elif q == 'dim':
        pcs = [r, r + 3, r + 6]
    else:
        pcs = [r, r + 4, r + 7]
    pcs = [p % 12 for p in pcs]
    return (r if bass is None else bass), pcs, r


# ---------------------------------------------------------------- the score
# a bar: (chord, melody) with melody a list of (note|'r', beats);
# a hemiola pair: ('H', [(chord, note), (chord, note), (chord, note)]) = two bars felt as 3/2
A1 = [  # a: bars 1-8, modal half cadence on Am
    ('Dm', [('D5', 2), ('A4', 1)]),
    ('G', [('B4', 2), ('C5', 1)]),
    ('Em', [('D5', 1.5), ('E5', .5), ('D5', 1)]),
    ('Am', [('C5', 3)]),
    ('Dm', [('F5', 2), ('E5', 1)]),
    ('G', [('D5', 1), ('B4', 1), ('G4', 1)]),
    ('C', [('E5', 1.5), ('D5', .5), ('C5', 1)]),
    ('Am', [('A4', 3)]),
]
A2 = [  # a': bars 9-16, climbs to A5, hemiola into the tonic
    ('Dm', [('D5', 2), ('A4', 1)]),
    ('G', [('B4', 2), ('D5', 1)]),
    ('Em', [('G5', 2), ('E5', 1)]),
    ('Am', [('A5', 2), ('G5', 1)]),
    ('F', [('F5', 1.5), ('E5', .5), ('C5', 1)]),
    ('H', [('G', 'D5'), ('C', 'E5'), ('Am', 'C5')]),
    ('Dm', [('D5', 3)]),
]
A3 = [  # b: bars 17-24, a rising sequence peaking on the B natural
    ('C', [('E5', .5), ('F5', .5), ('G5', 1), ('E5', 1)]),
    ('F', [('F5', .5), ('G5', .5), ('A5', 1), ('F5', 1)]),
    ('G', [('G5', .5), ('A5', .5), ('B5', 1), ('G5', 1)]),
    ('Dm', [('A5', 2), ('D5', 1)]),
    ('C', [('E5', 1), ('G5', 1), ('E5', 1)]),
    ('F', [('D5', 1), ('F5', 1), ('A5', 1)]),
    ('Em', [('G5', 1), ('E5', 1), ('B4', 1)]),
    ('G', [('B4', 2), ('C5', 1)]),
]
A4 = [  # a'': bars 25-32
    ('Dm', [('D5', 2), ('A4', 1)]),
    ('G', [('B4', 2), ('D5', 1)]),
    ('Em', [('E5', 2), ('G5', 1)]),
    ('Am', [('A5', 1.5), ('G5', .5), ('E5', 1)]),
    ('F', [('F5', 1), ('E5', 1), ('D5', 1)]),
    ('H', [('G', 'G5'), ('C', 'E5'), ('Am', 'C5')]),
    ('Dm', [('D5', 3)]),
]
TRANS = [  # bars 33-34: D minor turns into the dominant of B-flat; strings pick up
    ('Dm', [('r', 3)]),
    ('F', [('r', 2), ('C5', 1)]),
]
TRIO = [  # bars 35-66, B-flat Lydian (the E natural over C/Bb), strings, an octave down
    ('Bb', [('D5', 3)]),
    ('C/Bb', [('E5', 2), ('F5', 1)]),
    ('Bb', [('D5', 2), ('Bb4', 1)]),
    ('C/Bb', [('C5', 3)]),
    ('Gm', [('Bb4', 2), ('D5', 1)]),
    ('Am', [('C5', 2), ('E5', 1)]),
    ('F', [('A5', 2), ('G5', 1)]),
    ('C', [('E5', 3)]),
    ('Bb', [('F5', 2), ('D5', 1)]),
    ('C/Bb', [('E5', 2), ('G5', 1)]),
    ('Dm', [('A5', 2), ('F5', 1)]),
    ('C', [('G5', 3)]),
    ('Gm', [('F5', 1), ('D5', 1), ('Bb4', 1)]),
    ('H', [('Gm', 'D5'), ('C', 'E5'), ('F', 'C5')]),
    ('Bb', [('D5', 3)]),
    ('Gm', [('G5', 1), ('F5', 1), ('D5', 1)]),
    ('C', [('E5', 2), ('C5', 1)]),
    ('F', [('F5', 1), ('E5', 1), ('C5', 1)]),
    ('Bb', [('D5', 3)]),
    ('Gm', [('Bb5', 2), ('A5', 1)]),
    ('C', [('G5', 1), ('E5', 1), ('C5', 1)]),
    ('Am', [('A5', 2), ('G5', 1)]),
    ('Dm', [('F5', 3)]),
    ('Bb', [('D5', 2), ('F5', 1)]),
    ('C', [('E5', 2), ('G5', 1)]),
    ('Dm', [('A5', 2), ('F5', 1)]),
    ('C', [('E5', 3)]),
    ('H', [('Bb', 'D5'), ('C', 'E5'), ('G', 'B4')]),  # the B natural takes us home
    ('Dm', [('A4', 3)]),
    ('G', [('B4', 2), ('C5', 1)]),
]
CODA = [
    ('Dm', [('D5', 2), ('A4', 1)]),
    ('G', [('B4', 3)]),
    ('H', [('F', 'A4'), ('G', 'B4'), ('C', 'C5')]),
    ('Dm6', [('D5', 3)]),
    ('Dm6', [('r', 3)]),
]
INTRO = [('Dm', []), ('G', []), ('Dm', []), ('G', [])]

# ---------------------------------------------------------------- events
notes = []  # (ch, start tick, dur ticks, note, vel)


def add(ch, t, dur, n, vel):
    notes.append((ch, int(round(t)), max(6, int(round(dur))), n, max(1, min(127, int(vel)))))


def voice_chord(pcs, centre=62):
    """closed voicing of the chord tones nearest centre (G3..A4 region)"""
    out = []
    for p in pcs[:4]:
        best = min((n for n in range(48, 80) if n % 12 == p), key=lambda n: abs(n - centre))
        out.append(best)
    return sorted(out)


def bass_note(pc, lo=33):
    n = lo + ((pc - lo) % 12)
    return n


class Writer:
    def __init__(self):
        self.t = 0
        self.bar = 0
        self.ctr = 57

    def section(self, bars, mel_ch, oct=0, acc=1.0, mel_vel=100, counter=False, double=False,
                bass_vel=100, cresc=0):
        for bi, (c, mel) in enumerate(bars):
            if c == 'H':
                self.hemiola(mel, mel_ch, oct, acc, mel_vel, counter, double, bass_vel)
                continue
            bass, pcs, root = chord(c)
            fifth = (root + 7) % 12
            # oom: root, alternating with the fifth on even bars (not under a slash chord)
            b = bass if (self.bar % 2 == 0 or bass != root) else fifth
            add(CH_BASS, self.t, PPQ * 0.92, bass_note(b), bass_vel * acc)
            # pah-pah: closed chord, staccato, beat 2 leans a little harder than beat 3
            for beat, v in ((1, 74), (2, 64)):
                for n in voice_chord(pcs):
                    add(CH_CHORD, self.t + beat * PPQ, PPQ * 0.55, n, v * acc)
            self.melody(mel, mel_ch, oct, mel_vel + cresc * bi / max(1, len(bars)), double)
            if counter:
                self.counter_note(pcs, mel, self.t, 3 * PPQ)
            self.t += BAR
            self.bar += 1

    def melody(self, mel, ch, oct, vel, double, t0=None):
        t = self.t if t0 is None else t0
        for i, (n, beats) in enumerate(mel):
            if n != 'r':
                accent = 8 if (t - self.t) % BAR == 0 else 0
                nxt_rest = i + 1 < len(mel) and mel[i + 1][0] == 'r'
                legato = 0.8 if nxt_rest else 0.96
                add(ch, t, beats * PPQ * legato, midi(n) + oct, vel + accent)
                if double:
                    add(CH_CTR, t, beats * PPQ * legato, midi(n) + oct + 12, (vel + accent) * 0.7)
            t += beats * PPQ

    def counter_note(self, pcs, mel, t, dur):
        # guide-tone line: the chord tone nearest the previous one, avoiding the melody's pc
        mpc = midi(mel[0][0]) % 12 if mel and mel[0][0] != 'r' else None
        cands = [n for n in range(55, 70) if n % 12 in pcs and n % 12 != mpc]
        n = min(cands, key=lambda x: (abs(x - self.ctr), x))
        self.ctr = n
        add(CH_CTR, t, dur * 0.94, n, 78)

    def hemiola(self, halves, mel_ch, oct, acc, mel_vel, counter, double, bass_vel):
        # three half notes across two bars: bass, chord and melody all move together
        for k, (c, n) in enumerate(halves):
            bass, pcs, root = chord(c)
            t = self.t + k * 2 * PPQ
            add(CH_BASS, t, PPQ * 1.9, bass_note(bass), (bass_vel + 6) * acc)
            for cn in voice_chord(pcs):
                add(CH_CHORD, t, PPQ * 1.7, cn, 78 * acc)
            self.melody([(n, 2)], mel_ch, oct, mel_vel + 6, double, t0=t)
            if counter:
                self.counter_note(pcs, [(n, 2)], t, 2 * PPQ)
        self.t += 2 * BAR
        self.bar += 2


w = Writer()
w.section(INTRO, CH_MEL)
for part in (A1, A2, A3, A4):
    w.section(part, CH_MEL, cresc=10 if part is A3 else 0)
w.section(TRANS, CH_STR, oct=-12, acc=0.85, mel_vel=96)
w.section(TRIO, CH_STR, oct=-12, acc=0.8, mel_vel=104, bass_vel=88)
w.section(A1, CH_MEL, counter=True)
w.section(A2, CH_MEL, counter=True)
w.section(A3, CH_MEL, double=True, cresc=12)
w.section(A4, CH_MEL, double=True, mel_vel=106)
coda_start = w.t
w.section(CODA[:4], CH_MEL, mel_vel=94, acc=0.9)
# last bar: the Dm6 (B natural on top of the chord) left to ring, low D underneath
add(CH_BASS, w.t, BAR * 2, midi('D2'), 96)
for n in voice_chord(chord('Dm6')[1]):
    add(CH_CHORD, w.t, BAR * 1.8, n, 60)
add(CH_STR, w.t, BAR * 2, midi('D4'), 70)
add(CH_STR, w.t, BAR * 2, midi('A4'), 64)
w.t += BAR * 4  # the ringing bar, its tail, and a bar of reverb before the song loops
end = w.t

# ---------------------------------------------------------------- SMF writer


def vlq(x):
    b = [x & 0x7f]
    x >>= 7
    while x:
        b.append(0x80 | (x & 0x7f))
        x >>= 7
    return bytes(reversed(b))


def track(evs):
    evs = sorted(evs, key=lambda e: (e[0], e[1]))
    out = b''
    t = 0
    for tick, _, data in evs:
        out += vlq(tick - t) + data
        t = tick
    out += vlq(0) + b'\xff\x2f\x00'
    return b'MTrk' + struct.pack('>I', len(out)) + out


tempo = [(0, 0, b'\xff\x51\x03' + (60000000 // BPM).to_bytes(3, 'big')),
         (0, 0, b'\xff\x58\x04\x03\x02\x18\x08'),  # 3/4
         (0, 0, b'\xff\x03' + bytes([len(b'Raised Sixth')]) + b'Raised Sixth'),
         (end, 9, b'\xff\x01\x03end')]
trks = [track(tempo)]
for ch, (prog, vol, pan, rev) in SETUP.items():
    evs = [(0, 0, bytes([0xc0 | ch, prog])), (0, 1, bytes([0xb0 | ch, 7, vol])),
           (0, 1, bytes([0xb0 | ch, 10, pan])), (0, 1, bytes([0xb0 | ch, 91, rev]))]
    for c, t, d, n, v in notes:
        if c == ch:
            evs.append((t + d, 2, bytes([0x80 | ch, n, 0])))  # offs sort before ons at a tick
            evs.append((t, 3, bytes([0x90 | ch, n, v])))
    trks.append(track(evs))
hdr = b'MThd' + struct.pack('>IHHH', 6, 1, len(trks), PPQ)
open(sys.argv[1], 'wb').write(hdr + b''.join(trks))
print('bars %d, %.1f s, notes %d, end tick %d' % (end // BAR, end / (PPQ * BPM / 60), len(notes), end))
