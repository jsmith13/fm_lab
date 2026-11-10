#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Move DX7 patches between a .syx file and the FM-1 over USB-MIDI.

  python tools/syx.py send rhodes.syx        # file -> FM-1
  python tools/syx.py get bank mine.syx      # the FM-1 bank you are in (32 slots) -> file
  python tools/syx.py get voice one.syx      # the voice on screen -> file
  python tools/syx.py list                   # what the file holds, without sending it

A bulk dump (4104 bytes) replaces the 32 slots of the bank you are in (U1..U8: the voice on screen's,
or the last saved to). A single voice dump (163 bytes) lands in
the editor, unsaved, exactly as it would on a real DX7 -- press SAVE to keep it.

Many .syx files found online are a bank written as 32 back-to-back single-voice dumps rather
than one bulk dump. The FM-1 can only hold one frame at a time, so those are sent slowly and
only the last voice survives in the editor; convert such a file to a bulk dump, or send it and
save the voices you want one at a time.
"""
import sys
import time

import mido

BULK = (0x43, 0x00, 0x09, 0x20, 0x00)             # ..after F0: id, 0n, format, size hi, size lo
VOICE = (0x43, 0x00, 0x00, 0x01, 0x1B)
GAP = 0.3                                          # seconds between frames: the FM-1 holds one


# ---- receiving a bank on Windows
#
# A bulk dump is 4104 bytes. RtMidi -- which is what mido talks to here -- gives WinMM four 1024
# byte buffers for incoming SysEx, 4096 in all, and a message that does not fit is dropped with no
# error at all: you ask for the bank, the FM-1 says BANK SENT, and nothing arrives. The buffer size
# is fixed when RtMidi is compiled, so there is nothing to set. WinMM itself is happy to take one
# big buffer, so on Windows we ask it directly and leave mido to do the sending.
def _recv_winmm(name, seconds):
    import ctypes
    import ctypes.wintypes as w

    winmm = ctypes.WinDLL("winmm")
    DWORD_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong

    class MIDIHDR(ctypes.Structure):
        _fields_ = [("lpData", ctypes.c_char_p), ("dwBufferLength", w.DWORD),
                    ("dwBytesRecorded", w.DWORD), ("dwUser", DWORD_PTR), ("dwFlags", w.DWORD),
                    ("lpNext", ctypes.c_void_p), ("reserved", DWORD_PTR), ("dwOffset", w.DWORD),
                    ("dwReserved", DWORD_PTR * 8)]

    class MIDIINCAPS(ctypes.Structure):
        _fields_ = [("wMid", w.WORD), ("wPid", w.WORD), ("vDriverVersion", w.UINT),
                    ("szPname", w.WCHAR * 32), ("dwSupport", w.DWORD)]

    dev = -1
    for i in range(winmm.midiInGetNumDevs()):
        caps = MIDIINCAPS()
        winmm.midiInGetDevCapsW(DWORD_PTR(i), ctypes.byref(caps), ctypes.sizeof(caps))
        if name.startswith(caps.szPname):
            dev = i
            break
    if dev < 0:
        return None

    h = w.HANDLE()
    if winmm.midiInOpen(ctypes.byref(h), dev, 0, 0, 0):
        return None
    buf = ctypes.create_string_buffer(65536)
    hdr = MIDIHDR(lpData=ctypes.cast(buf, ctypes.c_char_p), dwBufferLength=len(buf))
    try:
        winmm.midiInPrepareHeader(h, ctypes.byref(hdr), ctypes.sizeof(hdr))
        winmm.midiInAddBuffer(h, ctypes.byref(hdr), ctypes.sizeof(hdr))
        winmm.midiInStart(h)
        yield None                                 # open and listening: the caller sends now
        end = time.time() + seconds
        while time.time() < end and not (hdr.dwFlags & 1):
            time.sleep(0.005)
        yield buf.raw[:hdr.dwBytesRecorded] if hdr.dwBytesRecorded else None
    finally:
        winmm.midiInStop(h)
        winmm.midiInReset(h)
        winmm.midiInUnprepareHeader(h, ctypes.byref(hdr), ctypes.sizeof(hdr))
        winmm.midiInClose(h)


def summarise(body):
    """One line about a SysEx message, given its bytes between F0 and F7."""
    size = f"{len(body) + 2:6d} B  "
    if len(body) == 3 and body[0] == 0x43 and (body[1] & 0xF0) == 0x20:
        return size + ("asks for a bank" if body[2] == 0x09 else "asks for a voice")
    if len(body) == 4102 and body[0] == 0x43 and body[2:5] == bytes(BULK[2:]):
        kind, n, at = "bulk dump, 32 voices", 4096, slice(118, 128)
    elif len(body) == 161 and body[0] == 0x43 and body[2:5] == bytes(VOICE[2:]):
        kind, n, at = "single voice", 155, slice(145, 155)
    else:
        return size + "not a DX7 voice dump -- the FM-1 will ignore it"
    data = body[5:5 + n]
    ok = (-sum(data)) & 0x7F == body[5 + n]
    name = bytes(b & 0x7F for b in data[at]).decode("ascii", "replace").strip()
    lead = "first is" if n == 4096 else "named"
    return size + f"{kind}, {'checksum ok' if ok else 'BAD CHECKSUM'}, {lead} {name!r}"


def frames(path):
    """Every F0..F7 message in a .syx file, as the bytes between the delimiters."""
    raw = open(path, "rb").read()
    out, i = [], 0
    while True:
        a = raw.find(b"\xf0", i)
        if a < 0:
            break
        b = raw.find(b"\xf7", a)
        if b < 0:
            print(f"  warning: {len(raw) - a} trailing bytes with no F7, dropped")
            break
        out.append(raw[a + 1:b])
        i = b + 1
    return out


def port_names():
    out = next((p for p in mido.get_output_names() if "FM-1" in p or "Felucca" in p), None)
    inp = next((p for p in mido.get_input_names() if "FM-1" in p or "Felucca" in p), None)
    if not out:
        sys.exit(f"no FM-1 in {mido.get_output_names()}")
    return out, inp


def send(path):
    msgs = frames(path)
    if not msgs:
        sys.exit(f"{path} holds no SysEx")
    for m in msgs:
        print("  " + summarise(m))
    out, _ = port_names()
    print(f"port: {out}")
    with mido.open_output(out) as port:
        for k, m in enumerate(msgs, 1):
            port.send(mido.Message("sysex", data=m))
            print(f"\r  sent {k} of {len(msgs)}", end="", flush=True)
            time.sleep(GAP)
    print("\ndone -- watch the FM-1's screen for BANK n RECEIVED or VOICE RECEIVED")


def get(what, path):
    out, inp = port_names()
    req = [0x43, 0x20, 0x09 if what == "bank" else 0x00]
    print(f"port: {out}")

    body = None
    rx = _recv_winmm(inp or "Felucca", 5.0) if sys.platform == "win32" and inp else None
    if rx is not None:
        next(rx)                                   # listening before the request goes out
        with mido.open_output(out) as tx:
            tx.send(mido.Message("sysex", data=req))
        body = next(rx)
        rx.close()
        if body is not None:
            body = body[1:-1] if body[:1] == b"\xf0" and body[-1:] == b"\xf7" else body
    else:
        if not inp:
            sys.exit(f"no FM-1 input in {mido.get_input_names()}")
        with mido.open_input(inp) as rxm, mido.open_output(out) as tx:
            for _ in rxm.iter_pending():            # drop anything already queued
                pass
            tx.send(mido.Message("sysex", data=req))
            end = time.time() + 5.0
            while time.time() < end and body is None:
                msg = rxm.poll()
                if msg is not None and msg.type == "sysex":
                    body = bytes(msg.data)
                time.sleep(0.005)

    if body is None:
        sys.exit("the FM-1 did not answer -- is it on, and is this firmware FM Lab?")
    print("  " + summarise(body))
    open(path, "wb").write(b"\xf0" + body + b"\xf7")
    print(f"wrote {path}")


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "send":
        send(a[1])
    elif len(a) == 2 and a[0] == "list":
        for m in frames(a[1]):
            print("  " + summarise(m))
    elif len(a) == 3 and a[0] == "get" and a[1] in ("bank", "voice"):
        get(a[1], a[2])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
