"""Function and control-flow discovery for an x86 PE image that has no relocation table.

Persona.exe ships without .reloc, so code pointers are found by other means:
  * recursive descent from the entry point (direct calls, jumps, conditional branches)
  * imm32 operands that point into .text (push offset f / mov [x], offset f: callbacks, WndProcs)
  * aligned dwords in the data sections that point at a plausible function start (vtables, thread procs)
  * MSVC switch tables (jmp [reg*4 + table]), bounded by the preceding `cmp reg, N; ja`
  * aligned starts after inter-function padding that nothing visibly references
  * tools/extra_entries.txt for targets found only at run time
"""
import os
import capstone
from capstone import x86 as X

LOOPS = ('loop', 'loope', 'loopne', 'jecxz', 'jcxz')
# instructions that MSVC 5/6 output never contains: decoding one means we are reading data
JUNK = {'aaa', 'aas', 'daa', 'das', 'aam', 'aad', 'bound', 'arpl', 'into', 'int1', 'hlt', 'salc', 'sldt', 'str',
        'in', 'out', 'insb', 'insd', 'insw', 'outsb', 'outsd', 'outsw', 'les', 'lds', 'retf', 'iretd', 'cli', 'sti',
        'ljmp', 'lcall', 'pushal', 'popal', 'xlatb', 'icebp', 'loope', 'loopne', 'por', 'jo', 'jno'}
NORETURN_NAMES ={'ExitProcess', 'ExitThread', 'RaiseException', 'TerminateProcess', '_exit', 'exit', 'abort'}


class Discovery:
    def __init__(self, img, extra_file=None):
        self.img = img
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self._cache = {}
        self.insns = {}              # address -> capstone insn (every decoded instruction)
        self.entries = set()         # function entry points
        self.jump_tables = {}        # address of `jmp [..]` -> list of targets
        self.table_bytes = set()     # bytes in .text that are data (jump tables, index tables)
        self.code_imms = set()       # code addresses used as immediates
        self.data_ptrs = set()       # code addresses stored in data sections
        self.noreturn_iat = {a for a, (d, n) in img.imports.items() if n in NORETURN_NAMES}
        self.extra = []
        if extra_file and os.path.exists(extra_file):
            for line in open(extra_file):
                line = line.split('#')[0].strip()
                if line:
                    self.extra.append(int(line, 16))

    # ------------------------------------------------------------ decoding
    def dis(self, a):
        if a in self._cache:
            return self._cache[a]
        try:
            i = next(self.md.disasm(self.img.bytes_at(a, 16), a))
        except StopIteration:
            i = None
        self._cache[a] = i
        return i

    @staticmethod
    def groups(i):
        g = set(i.groups)
        if i.mnemonic in LOOPS:
            g.add(X.X86_GRP_JUMP)
        return g

    # ------------------------------------------------------------ switch tables
    def _bound_before(self, a, reg):
        """Look back a few instructions for `cmp reg, N` (+ ja) and return N+1 entries, or None."""
        p = a
        for _ in range(8):
            prev = None
            for back in range(1, 16):
                q = p - back
                i = self.insns.get(q)
                if i is not None and q + i.size == p:
                    prev = i
                    break
            if prev is None:
                return None
            if prev.mnemonic == 'cmp' and len(prev.operands) == 2:
                d, s = prev.operands
                if d.type == X.X86_OP_REG and s.type == X.X86_OP_IMM:
                    return (s.imm & 0xffffffff) + 1
            p = prev.address
        return None

    def _byte_index_table(self, a, reg):
        """`mov r8, byte [idx + tbl]` / `movzx r, byte [idx + tbl]` feeding the jump: mark the byte table."""
        p = a
        for _ in range(6):
            prev = None
            for back in range(1, 16):
                q = p - back
                i = self.insns.get(q)
                if i is not None and q + i.size == p:
                    prev = i
                    break
            if prev is None:
                return None
            if prev.mnemonic in ('mov', 'movzx') and len(prev.operands) == 2:
                s = prev.operands[1]
                if s.type == X.X86_OP_MEM and s.size == 1 and self.img.in_code(s.mem.disp & 0xffffffff):
                    tbl = s.mem.disp & 0xffffffff
                    n = self._bound_before(prev.address, None)
                    return tbl, n
            p = prev.address
        return None

    def read_table(self, jmp_insn, tbl):
        n = self._bound_before(jmp_insn.address, None)
        bi = self._byte_index_table(jmp_insn.address, None)
        if bi is not None:
            btbl, bn = bi
            if bn:
                vals = self.img.bytes_at(btbl, bn)
                self.table_bytes.update(range(btbl, btbl + bn))
                n = max(vals) + 1 if n is None else n
        out = []
        p = tbl
        limit = n if n is not None else 1024
        while len(out) < limit:
            if not self.img.in_code(p):
                break
            t = self.img.u32(p)
            if not self.img.in_code(t):
                break
            if n is None and (p in self.insns or p in self.entries):
                break
            out.append(t)
            self.table_bytes.update(range(p, p + 4))
            p += 4
        return out

    # ------------------------------------------------------------ recursive descent
    def explore(self, starts):
        work = list(starts)
        while work:
            a = work.pop()
            while self.img.in_code(a) and a not in self.insns and a not in self.table_bytes:
                i = self.dis(a)
                if i is None:
                    break
                self.insns[a] = i
                g = self.groups(i)
                m = i.mnemonic
                # immediates that point into code are candidate function pointers
                for op in i.operands:
                    if op.type == X.X86_OP_IMM and not (X.X86_GRP_JUMP in g or X.X86_GRP_CALL in g):
                        v = op.imm & 0xffffffff
                        if self.img.in_code(v):
                            self.code_imms.add(v)
                if X.X86_GRP_JUMP in g or X.X86_GRP_CALL in g:
                    op = i.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if X.X86_GRP_CALL in g:
                            self.entries.add(t)
                        work.append(t)
                    elif m == 'jmp' and op.type == X.X86_OP_MEM and op.mem.index and op.mem.scale == 4 \
                            and not op.mem.base and self.img.in_code(op.mem.disp & 0xffffffff):
                        tg = self.read_table(i, op.mem.disp & 0xffffffff)
                        self.jump_tables[a] = tg
                        work.extend(tg)
                    elif X.X86_GRP_CALL in g and op.type == X.X86_OP_MEM and not op.mem.base \
                            and not op.mem.index and (op.mem.disp & 0xffffffff) in self.noreturn_iat:
                        break
                    if m == 'jmp':
                        break
                if m in ('ret', 'retf', 'hlt', 'int3'):
                    break
                a += i.size

    # ------------------------------------------------------------ heuristics
    def plausible_start(self, a, aligned=True):
        """Does a look like the first instruction of a function (not mid-instruction, not data)?
        aligned: require MSVC's 16-byte function alignment (for seeds nothing points at)."""
        if not self.img.in_code(a) or a in self.table_bytes:
            return False
        if a in self.entries:
            return True
        # MSVC aligns functions to 16 bytes and pads with nop (0x90) after a ret / jmp; hand-written
        # CRT routines follow a ret (c3) or ret n (c2 nn 00) directly
        boundary = self.img.u8(a - 1) in (0x90, 0xcc, 0xc3) or             (self.img.u8(a - 3) == 0xc2 and self.img.u8(a - 1) == 0) or (a - 1) in self.table_bytes
        if not boundary or (aligned and a % 16 != 0):
            return False
        if self.img.u8(a) in (0x90, 0xcc, 0x00):
            return False
        # trial decode along the fall-through path: no instruction MSVC never emits, ends in ret/jmp
        p = a
        for _ in range(64):
            i = self.dis(p)
            if i is None or i.mnemonic in JUNK or p in self.table_bytes:
                return False
            if i.mnemonic in ('ret', 'jmp'):
                return True
            p += i.size
        return False

    def run(self):
        img = self.img
        self.entries.add(img.entry)
        self.explore([img.entry] + self.extra)
        self.entries.update(self.extra)
        # data-section code pointers (vtables, callbacks, thread procs)
        cands = set()
        for nm, s, e, ch in img.sections:
            if ch & 0x20000000:
                continue
            for p in range(s, e - 3, 4):
                if p - img.base + 4 > img.size:
                    break
                v = img.u32(p)
                if img.in_code(v):
                    cands.add(v)
        # iterate: code immediates and data pointers become entries once they look like function starts
        done = set()
        while True:
            new = [v for v in (cands | self.code_imms) - done if self.plausible_start(v, aligned=False)]
            done |= cands | self.code_imms
            if not new:
                break
            for v in new:
                if v in self.code_imms:
                    pass
                self.entries.add(v)
            self.data_ptrs |= set(v for v in new if v in cands)
            self.explore(sorted(new))
        # gap seeds: aligned starts after padding that nothing references (dead code or computed calls)
        gaps = []
        for b, e in img.code:
            for a in range((b + 15) & ~15, e, 16):
                if a in self.insns or a in self.table_bytes:
                    continue
                if img.u8(a - 1) not in (0x90, 0xcc, 0xc3, 0xc2):
                    continue
                if img.u8(a) in (0x90, 0xcc, 0x00):
                    continue
                if any(x in self.table_bytes for x in range(a, a + 8)):
                    continue
                if self.plausible_start(a):
                    gaps.append(a)
        self.explore(gaps)
        self.entries.update(a for a in gaps if a in self.insns)
        # incremental-link thunks: an entry that is only `jmp target` makes the target an entry too
        changed = True
        while changed:
            changed = False
            for e in list(self.entries):
                i = self.dis(e)
                if i and i.mnemonic == 'jmp' and i.operands[0].type == X.X86_OP_IMM:
                    t = i.operands[0].imm & 0xffffffff
                    if img.in_code(t) and t not in self.entries:
                        self.entries.add(t)
                        changed = True
        self.entries = {e for e in self.entries if e in self.insns}
        return self


if __name__ == '__main__':
    import sys, collections
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from image import Image
    img = Image(sys.argv[1])
    d = Discovery(img, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'extra_entries.txt')).run()
    code_bytes = sum(e - b for b, e in img.code)
    covered = sum(i.size for i in d.insns.values())
    print('functions %d, instructions %d, jump tables %d' % (len(d.entries), len(d.insns), len(d.jump_tables)))
    print('code bytes covered %d / %d (%.1f%%)' % (covered, code_bytes, 100.0 * covered / code_bytes))
    hist = collections.Counter(i.mnemonic for i in d.insns.values())
    print('distinct mnemonics %d' % len(hist))
    print(' '.join('%s:%d' % kv for kv in sorted(hist.items())))
