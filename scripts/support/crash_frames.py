#!/usr/bin/env python3
"""crash_frames.py LOG [BINARY ...] -- name the frames in a support log's crash and frozen-UI records.

LOG is a kept log (eclipse-*.log), the one-file bundle (eclipse-logs.txt) or the share page's all.txt.
BINARY is the EXACT build the log came from -- its sha256 is in the log's "Binary:" line -- as shipped
(every package keeps its symbols): moonlight-tv.exe (Windows), moonlight-tv (Linux), bin/aurora from the
.ipk (webOS); add any of the package's own .so files to name frames in those too.

Reads the two record kinds src/app/util/crash_log.c writes:
  === CRASH: ...               a crash (signal / exception)       ... === END CRASH ===
  === UI THREAD STUCK: ...     the UI thread frozen 5 s or more   ... === END UI THREAD STUCK ===
and resolves, per platform:
  Windows   "#NN module+0xOFF"                     -> function+offset (PE ImageBase + OFF)
  Linux     "path(+0xOFF) [0xADDR]" backtrace lines -> function+offset
  webOS     pc / lr and every raw stack word that falls in a given binary's code, through the record's
            /proc/self/maps (32-bit ARM backtraces come out empty; the stack words are the walk)
Needs only binutils' nm (it reads PE, x86-64 and ARM ELF alike).
"""
import bisect
import os
import re
import subprocess
import sys

MAPS_RE = re.compile(r'^([0-9a-f]+)-([0-9a-f]+) (\S{4}) ([0-9a-f]+) \S+ \d+\s*(.*)$')
WIN_FRAME_RE = re.compile(r'^#(\d+) (\S+?)\+0x([0-9a-f]+)')
LINUX_FRAME_RE = re.compile(r'^(\S+)\((\S*?)\+0x([0-9a-f]+)\) \[0x([0-9a-f]+)\]')
HEX_RE = re.compile(r'0x([0-9a-f]+)')


class Binary:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        with open(path, 'rb') as f:
            head = f.read(64)
        self.pe = head[:2] == b'MZ'
        self.elf_exec = head[:4] == b'\x7fELF' and int.from_bytes(head[16:18], 'little') == 2
        self.arm = head[:4] == b'\x7fELF' and int.from_bytes(head[18:20], 'little') == 40   # EM_ARM: Thumb bit
        self.image_base = 0
        if self.pe:
            out = subprocess.run(['objdump', '-p', path], capture_output=True, text=True).stdout
            m = re.search(r'ImageBase\s+([0-9a-fA-F]+)', out)
            self.image_base = int(m.group(1), 16) if m else 0x140000000
        # -S: ELF symbols carry their size, so an address past a function's end (data in the code segment, a
        # stale stack word) is not given a name it does not have. PE symbols have none: nearest symbol below.
        out = subprocess.run(['nm', '-n', '-S', '--defined-only', path], capture_output=True, text=True).stdout
        self.syms = []
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 4 and parts[2] in 'tTwW' and not parts[3].startswith('$'):
                self.syms.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
            elif len(parts) == 3 and parts[1] in 'tTwW' and not parts[2].startswith('$'):
                self.syms.append((int(parts[0], 16), None, parts[2]))
        self.syms.sort()
        self.keys = [a for a, _, _ in self.syms]

    def name_of(self, vaddr):
        i = bisect.bisect_right(self.keys, vaddr) - 1
        while i >= 0 and self.syms[i][1] == 0 and self.syms[i][0] == vaddr:   # zero-size labels at the same spot
            i -= 1
        if i < 0:
            return None
        start, size, name = self.syms[i]
        if size is not None and vaddr >= start + size:
            return None
        if size is None and not self.pe and vaddr != start:   # an ELF label without a size (_init, _fini)
            return None
        return '%s+0x%x' % (name, vaddr - start)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    bins = {}
    for p in sys.argv[2:]:
        b = Binary(p)
        bins[b.name] = b
        if b.name.endswith('.exe'):
            bins[b.name[:-4]] = b
    lines = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r', '').splitlines()
    lines = [re.sub(r'\x1b\[[0-9;]*m', '', l) for l in lines]

    # Pass 1: each record, and the maps that belong to it (a later stall record in the same run says "as in
    # this run's first record", so the last maps seen in the current file section is reused).
    records, maps, cur = [], [], None
    for l in lines:
        if l.startswith('===== '):   # a new file in a bundle: a new run
            maps = []
        if l.startswith('=== CRASH') or l.startswith('=== UI THREAD STUCK'):
            cur = {'head': l, 'body': [], 'maps': []}
            continue
        if cur is not None:
            if l.startswith('=== END'):
                if cur['maps']:
                    maps = cur['maps']
                else:
                    cur['maps'] = maps
                records.append(cur)
                cur = None
                continue
            m = MAPS_RE.match(l)
            if m:
                cur['maps'].append((int(m.group(1), 16), int(m.group(2), 16), m.group(3), int(m.group(4), 16),
                                    m.group(5).strip()))
            else:
                cur['body'].append(l)
    if not records:
        print('no crash or frozen-UI records in', sys.argv[1])
        return 1

    def via_maps(addr, rec, return_address=False):
        for start, end, perms, off, path in rec['maps']:
            if start <= addr < end and 'x' in perms:
                name = os.path.basename(path) if path else '(anonymous)'
                b = bins.get(name)
                if b is None:
                    return '%s+0x%x' % (name, addr - start + off), False
                if b.elf_exec:
                    vaddr = addr
                else:   # ET_DYN: the load base is where the file's offset-0 mapping starts
                    base = min((s for s, e, p, o, fp in rec['maps'] if fp == path and o == 0), default=start - off)
                    vaddr = addr - base
                look = vaddr & ~1 if b.arm else vaddr   # ARM: bit 0 is the Thumb flag, not part of the address
                look = look - 1 if return_address else look   # a return address: land inside the call
                return '%s: %s' % (name, b.name_of(look) or '?'), True
        return None, False

    for rec in records:
        print(rec['head'])
        head = rec['head']
        for key in ('pc', 'lr'):
            m = re.search(r'\b%s 0x([0-9a-f]+)' % key, head)
            if m is None:
                for l in rec['body']:
                    m = re.search(r'\b%s 0x([0-9a-f]+)' % key, l)
                    if m:
                        break
            if m:
                where, _ = via_maps(int(m.group(1), 16), rec, return_address=(key == 'lr'))
                if where:
                    print('  %s 0x%s = %s' % (key, m.group(1), where))
        in_words = False
        word_index = 0
        for l in rec['body']:
            if l.startswith('--- stack words'):
                in_words = True
                continue
            if l.startswith('---'):
                in_words = False
            if in_words:
                for h in HEX_RE.findall(l):
                    v = int(h, 16)
                    where, ours = via_maps(v, rec, return_address=True)
                    if ours and not where.endswith(': ?'):
                        print('  stack word %3d 0x%x = %s   (possible return address)' % (word_index, v, where))
                    word_index += 1
                continue
            m = WIN_FRAME_RE.match(l)
            if m:
                b = bins.get(m.group(2)) or bins.get(m.group(2).lower())
                if b is not None and b.pe:
                    back = 0 if int(m.group(1)) == 0 else 1   # #00 is where it stopped; the rest are return addresses
                    print('  #%s %s+0x%s = %s' % (m.group(1), m.group(2), m.group(3),
                                                 b.name_of(b.image_base + int(m.group(3), 16) - back) or '?'))
                else:
                    print('  ' + l)
                continue
            m = LINUX_FRAME_RE.match(l)
            if m:
                name = os.path.basename(m.group(1))
                b = bins.get(name)
                if b is not None and m.group(2) == '':
                    print('  %s(+0x%s) = %s' % (name, m.group(3), b.name_of(int(m.group(3), 16) - 1) or '?'))
                else:
                    print('  %s(%s+0x%s)' % (name, m.group(2), m.group(3)))
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
