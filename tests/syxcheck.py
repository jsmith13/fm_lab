"""Did the bank and the .syx import actually move the right bytes?

Pairs with tests/bank.txt. The screenshots show what the screen claimed; this reads the files the
run left behind and the sound it made, and checks the claims against them.

The strongest check here is the last one: the chord played from the editor after a voice arrived
over MIDI, and the chord played after that voice had been packed, written to flash, read back and
unpacked, have to be the same samples. Anything lost in the round trip shows up as a difference.

    python3 tests/syxcheck.py build/host/bank
"""
import struct
import sys

sys.path.insert(0, 'tests')
import mksyx                                        # noqa: E402  (the fixtures, so we can compare)

prefix = sys.argv[1] if len(sys.argv) > 1 else 'build/host/bank'
FS = 44100
fails = []


def check(name, ok, detail=''):
    print('%-46s %s%s' % (name, 'ok' if ok else '<-- WRONG', '  ' + detail if detail else ''))
    if not ok:
        fails.append(name)


def name_of(packed):
    return bytes(packed[118:128]).decode('ascii', 'replace').rstrip()


# ---- the bank sector, as the firmware left it
bank = open(prefix + '-bank.bin', 'rb').read()
check('bank is 32 packed voices', len(bank) == 4096, '%d B' % len(bank))
check('slot 1 is TEST 01', name_of(bank[0:128]) == 'TEST 01', name_of(bank[0:128]))
check('slot 5 is the voice that arrived', name_of(bank[4 * 128:5 * 128]) == 'ONE VOICE',
      name_of(bank[4 * 128:5 * 128]))
check('slot 5 is byte-for-byte what was sent', bank[4 * 128:5 * 128] == mksyx.pack(mksyx.one_voice()))
check('slot 32 survived the damaged bank', name_of(bank[31 * 128:32 * 128]) == 'TEST 32',
      name_of(bank[31 * 128:32 * 128]))

# ---- naming. TEST 01 was saved to slot 20 after two letters of it were turned into BEST 02, and
# then to slot 21 after a name was typed and the save backed out of, which must leave no trace.
check('slot 20 took the name that was typed', name_of(bank[19 * 128:20 * 128]) == 'BEST 02',
      name_of(bank[19 * 128:20 * 128]))
check('the sound itself is still TEST 01',
      bank[19 * 128:20 * 128 - 10] == mksyx.pack(mksyx.test_bank()[0])[:118])
check('backing out of a save forgets the letters', name_of(bank[20 * 128:21 * 128]) == 'BEST 02',
      name_of(bank[20 * 128:21 * 128]))
check('so the two slots are the same voice', bank[19 * 128:20 * 128] == bank[20 * 128:21 * 128])


# ---- what the FM-1 sent back when asked
raw = open(prefix + '-out.syx', 'rb').read()
frames, i = [], 0
while i < len(raw):
    if raw[i] != 0xF0:
        i += 1
        continue
    j = raw.index(0xF7, i)
    frames.append(raw[i + 1:j])
    i = j + 1
check('four dumps came back', len(frames) == 4, '%d' % len(frames))
if len(frames) == 4:
    blk, voc = frames[:2]
    check('the bulk dump is a 32-voice dump', len(blk) == 4102 and blk[:5] == b'\x43\x00\x09\x20\x00')
    check('its checksum is right', len(blk) == 4102 and mksyx.checksum(blk[5:4101]) == blk[4101])
    check('it is the bank in flash', blk[5:4101] == bank)
    check('the voice dump is a one-voice dump', len(voc) == 161 and voc[:5] == b'\x43\x00\x00\x01\x1b')
    check('its checksum is right', len(voc) == 161 and mksyx.checksum(voc[5:160]) == voc[160])
    want = bytearray(mksyx.test_bank()[0])      # TEST 01, as renamed by the naming steps above
    want[145:155] = b'BEST 02   '
    check('it is the voice on screen, renamed', bytes(voc[5:160]) == bytes(want))

# ---- the other banks
test01 = mksyx.pack(mksyx.test_bank()[0])
bank2 = open(prefix + '-bank2.bin', 'rb').read()
check('a bank sent on U2 went into U2', bank2[0:128] == test01 and name_of(bank2[31 * 128:]) == 'TEST 32',
      name_of(bank2[0:128]))
bank3 = open(prefix + '-bank3.bin', 'rb').read()
check('U3-05 is TEST 01, saved from U2-01', bank3[4 * 128:5 * 128] == test01, name_of(bank3[4 * 128:5 * 128]))
if len(frames) == 4:
    check('the second bulk dump is U3', bytes(frames[2][5:4101]) == bank3)
    check('its checksum is right', mksyx.checksum(frames[2][5:4101]) == frames[2][4101])

import re                                           # noqa: E402
log = open(prefix + '.log', errors='replace').read()
sent = [(int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4)))
        for m in re.finditer(r'^\s*(\d+) ms  MIDI out (B0|C0) (\d+) (\d+)', log, re.M)]


def landed(t):
    """Where PRESETS went at t ms, by what it sent: (bank, program)."""
    b = [s[3] for s in sent if s[0] == t and s[1] == 'B0' and s[2] == 0]
    p = [s[2] for s in sent if s[0] == t and s[1] == 'C0']
    return (b[-1] if b else None, p[-1] if p else None)


check('SEL + PRESETS: U1-21 to U2-01', landed(10400) == (2, 0), str(landed(10400)))
check('SEL + PRESETS back: U3-05 to U3-01', landed(13900) == (3, 0), str(landed(13900)))
check('..then to U2-01', landed(14000) == (2, 0), str(landed(14000)))
check('..then three back, round to U8-01', landed(14100) == (8, 0), str(landed(14100)))
check('PRESETS onto U1-01 sends bank 1, program 0', landed(800) == (1, 0), str(landed(800)))

# ---- the sound
data = open(prefix + '.wav', 'rb').read()
i = 12
while i < len(data):
    cid, sz = struct.unpack_from('<4sI', data, i)
    if cid == b'data':
        body = data[i + 8:i + 8 + sz]
        break
    i += 8 + sz + (sz & 1)
CH = struct.unpack_from('<H', data, 22)[0]
pcm = struct.unpack('<%dh' % (len(body) // 2), body)


def window(t_on, t_end):
    """The samples of a chord, from its first sound. The window starts before the key goes down,
    because the block the note lands in begins a millisecond or two early and two chords 1.5 s
    apart do not share a block boundary -- start at the key and you start at a different phase."""
    a, b = int((t_on - 200) * FS / 1000) * CH, int(t_end * FS / 1000) * CH
    s = pcm[a:b]
    for k, v in enumerate(s):
        if v:
            return s[k:]
    return ()


a_sine = window(1000, 1850)          # TEST 01, out of the bank that arrived
b_edit = window(2500, 3350)          # ONE VOICE, in the editor, straight off the wire
c_slot = window(5000, 5850)          # ONE VOICE again, after a trip through flash
d_sine = window(6700, 7550)          # TEST 01 again, after the damaged bank was refused

n = min(len(a_sine), len(b_edit), len(c_slot), len(d_sine))
check('all four chords made a sound', n > FS // 2 * CH, '%d samples' % n)
check('saved and reloaded is the same sound', b_edit[:n] == c_slot[:n])
check('the refused bank changed nothing', a_sine[:n] == d_sine[:n])
check('the two voices are not the same sound', a_sine[:n] != b_edit[:n])

print('\n%d checks failed' % len(fails))
sys.exit(1 if fails else 0)
