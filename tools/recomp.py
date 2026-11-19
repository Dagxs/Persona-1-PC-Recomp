#!/usr/bin/env python3
"""Static recompiler: Persona.exe (1999 Japanese Windows port of Megami Ibunroku Persona) -> C.

Model
  * Guest registers, lazy flags and the x87 stack are globals (runtime/cpu.h). Guest threads are
    serialised by the runtime, which swaps this state on a thread switch.
  * Guest memory is one flat array; the PE image is mapped at its preferred base (0x400000).
  * Every function entry becomes `void f_XXXXXXXX(void)`. `call` pushes the guest return address
    and calls the C function; `ret` pops it and returns. A jump to another entry is a tail call.
    Indirect branches and imported functions go through rt_call()/rt_jump(), which also route
    "magic" addresses (Win32 imports, COM methods) to host code.
usage: recomp.py Persona.exe outdir
"""
import os
import sys
import struct
import zlib
import collections
from capstone import x86 as X

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from image import Image
from discover import Discovery, LOOPS

REG32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REG = {}
for k, r in enumerate(REG32):
    REG[r] = (k, 32)
    REG[r[1:]] = (k, 16)
for k, r in enumerate(['al', 'cl', 'dl', 'bl']):
    REG[r] = (k, 8)
for k, r in enumerate(['ah', 'ch', 'dh', 'bh']):
    REG[r] = (k, 'hi')

COND = {
    'o': 'cc_o()', 'no': '!cc_o()', 'b': 'cc_b()', 'ae': '!cc_b()', 'e': 'cc_z()', 'ne': '!cc_z()',
    'be': 'cc_be()', 'a': '!cc_be()', 's': 'cc_s()', 'ns': '!cc_s()', 'p': 'cc_p()', 'np': '!cc_p()',
    'l': 'cc_l()', 'ge': '!cc_l()', 'le': 'cc_le()', 'g': '!cc_le()',
}
ARITH = {'add': 'op_add', 'adc': 'op_adc', 'sub': 'op_sub', 'sbb': 'op_sbb',
         'and': 'op_and', 'or': 'op_or', 'xor': 'op_xor', 'cmp': 'op_sub', 'test': 'op_and'}
SHIFT = {'shl': 'op_shl', 'sal': 'op_shl', 'shr': 'op_shr', 'sar': 'op_sar',
         'rol': 'op_rol', 'ror': 'op_ror', 'rcl': 'op_rcl', 'rcr': 'op_rcr'}


class Unsupported(Exception):
    pass


def mask(sz):
    return (1 << sz) - 1


class Gen:
    def __init__(self, img, disc):
        self.img = img
        self.d = disc
        self.unimpl = collections.Counter()

    # ------------------------------------------------------------ operands
    def reg_r(self, name):
        if name not in REG:
            raise Unsupported('register ' + name)
        k, sz = REG[name]
        if sz == 32:
            return 'R[%d]' % k
        if sz == 16:
            return '((uint16_t)R[%d])' % k
        if sz == 8:
            return '((uint8_t)R[%d])' % k
        return '((uint8_t)(R[%d] >> 8))' % k

    def reg_w(self, name, v):
        if name not in REG:
            raise Unsupported('register ' + name)
        k, sz = REG[name]
        if sz == 32:
            return 'R[%d] = (uint32_t)(%s);' % (k, v)
        if sz == 16:
            return 'R[%d] = (R[%d] & 0xffff0000u) | (uint16_t)(%s);' % (k, k, v)
        if sz == 8:
            return 'R[%d] = (R[%d] & 0xffffff00u) | (uint8_t)(%s);' % (k, k, v)
        return 'R[%d] = (R[%d] & 0xffff00ffu) | ((uint32_t)(uint8_t)(%s) << 8);' % (k, k, v)

    def addr(self, i, op):
        m = op.mem
        parts = []
        if m.base:
            bn = i.reg_name(m.base)
            if bn == 'eip':
                parts.append('0x%xu' % (i.address + i.size))
            else:
                if REG[bn][1] != 32:
                    raise Unsupported('16-bit addressing')
                parts.append(self.reg_r(bn))
        if m.index:
            xn = i.reg_name(m.index)
            parts.append(self.reg_r(xn) if m.scale == 1 else '%s*%du' % (self.reg_r(xn), m.scale))
        if m.disp or not parts:
            parts.append('0x%xu' % (m.disp & 0xffffffff))
        e = '(uint32_t)(%s)' % ' + '.join(parts)
        if m.segment:
            sn = i.reg_name(m.segment)
            if sn == 'fs':
                e = '(FS_BASE + %s)' % e
            elif sn not in ('ds', 'ss', 'es', 'cs'):
                raise Unsupported('segment ' + sn)
        return e

    def rd(self, i, op, sz=None):
        if op.type == X.X86_OP_REG:
            return self.reg_r(i.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return '0x%xu' % (op.imm & mask(sz or op.size * 8))
        if op.type == X.X86_OP_MEM:
            return 'RD%d(%s)' % (sz or op.size * 8, self.addr(i, op))
        raise Unsupported('operand')

    def wr(self, i, op, v, sz=None):
        if op.type == X.X86_OP_REG:
            return self.reg_w(i.reg_name(op.reg), v)
        if op.type == X.X86_OP_MEM:
            return 'WR%d(%s, %s);' % (sz or op.size * 8, self.addr(i, op), v)
        raise Unsupported('write operand')

    # ------------------------------------------------------------ control flow helpers
    def is_entry(self, t):
        return t in self.d.entries

    def goto(self, fn, t, cur):
        if t in fn.body and (t == fn.ep or not self.is_entry(t)):
            return ('{ POLLAT(0x%xu); goto L_%x; }' % (cur, t)) if t <= cur else 'goto L_%x;' % t
        if self.is_entry(t):
            return '{ f_%08x(); return; }' % t
        return 'rt_jump(0x%xu); return;' % t

    # ------------------------------------------------------------ one function
    class Fn:
        pass

    def collect(self, ep):
        fn = self.Fn()
        fn.ep = ep
        fn.body = {}
        work = [ep]
        while work:
            a = work.pop()
            while a in self.d.insns and a not in fn.body:
                if a != ep and self.is_entry(a):
                    break
                i = self.d.insns[a]
                fn.body[a] = i
                g = Discovery.groups(i)
                m = i.mnemonic
                if X.X86_GRP_JUMP in g:
                    op = i.operands[0]
                    if op.type == X.X86_OP_IMM:
                        work.append(op.imm & 0xffffffff)
                    elif a in self.d.jump_tables:
                        work.extend(self.d.jump_tables[a])
                    if m == 'jmp':
                        break
                if m in ('ret', 'hlt', 'int3'):
                    break
                if m == 'call' and i.operands[0].type == X.X86_OP_MEM and not i.operands[0].mem.base \
                        and not i.operands[0].mem.index and (i.operands[0].mem.disp & 0xffffffff) in self.d.noreturn_iat:
                    break
                a += i.size
        return fn

    def emit_fn(self, ep):
        fn = self.collect(ep)
        out = ['void f_%08x(void) {' % ep, '  FN_ENTER(0x%xu);' % ep]
        addrs = sorted(fn.body)
        if addrs[0] != ep:
            out.append('  goto L_%x;' % ep)
        for k, a in enumerate(addrs):
            i = fn.body[a]
            out.append('L_%x:' % a)
            try:
                if a in PATCHES:
                    lines, falls = PATCHES[a], True
                else:
                    lines, falls = self.insn(fn, i)
            except (Unsupported, KeyError, IndexError) as ex:
                self.unimpl['%s (%s)' % (i.mnemonic, ex)] += 1
                lines, falls = ['rt_unimpl(0x%xu, "%s %s");' % (a, i.mnemonic, i.op_str)], False
            out.extend('  ' + l for l in lines)
            nxt = a + i.size
            if falls and not (k + 1 < len(addrs) and addrs[k + 1] == nxt):
                out.append('  ' + self.goto(fn, nxt, a))
        out.append('}')
        return out

    # ------------------------------------------------------------ instructions
    def insn(self, fn, i):
        m = i.mnemonic
        ops = i.operands
        a = i.address
        nxt = a + i.size
        rep = ''
        if m.startswith(('rep ', 'repe ', 'repne ')):
            rep, m = m.split(' ', 1)
        if m.startswith('lock '):
            m = m[5:]
        if m in ('nop', 'wait', 'fwait', 'fnclex', 'fclex', 'cld') and m != 'cld':
            return [], True
        if m == 'cld':
            return ['DF = 0;'], True
        if m == 'std':
            return ['DF = 1;'], True

        if m == 'mov':
            d, s = ops
            return [self.wr(i, d, self.rd(i, s))], True
        if m == 'movzx':
            d, s = ops
            return [self.wr(i, d, self.rd(i, s))], True
        if m == 'movsx':
            d, s = ops
            return [self.wr(i, d, '(uint32_t)(int32_t)(int%d_t)%s' % (s.size * 8, self.rd(i, s)))], True
        if m == 'lea':
            d, s = ops
            e = self.addr(i, s)
            if e.startswith('(FS_BASE'):
                raise Unsupported('lea fs')
            return [self.wr(i, d, e)], True
        if m == 'xchg':
            d, s = ops
            return ['{ uint32_t t1 = %s, t2 = %s; %s %s }' % (self.rd(i, d), self.rd(i, s),
                                                           self.wr(i, d, 't2'), self.wr(i, s, 't1'))], True
        if m == 'push':
            s = ops[0]
            if s.type == X.X86_OP_IMM:
                return ['PUSH32(0x%xu);' % (s.imm & 0xffffffff)], True
            if s.size == 2:
                return ['PUSH16(%s);' % self.rd(i, s)], True
            return ['{ uint32_t t = %s; PUSH32(t); }' % self.rd(i, s)], True
        if m == 'pop':
            d = ops[0]
            if d.size == 2:
                return ['{ uint32_t t = POP16(); %s }' % self.wr(i, d, 't')], True
            return ['{ uint32_t t = POP32(); %s }' % self.wr(i, d, 't')], True
        if m == 'pushfd':
            return ['PUSH32(eflags_get());'], True
        if m == 'popfd':
            return ['eflags_set(POP32());'], True
        if m == 'pushal':
            return ['{ uint32_t t = ESP; PUSH32(EAX); PUSH32(ECX); PUSH32(EDX); PUSH32(EBX); PUSH32(t); PUSH32(EBP); PUSH32(ESI); PUSH32(EDI); }'], True
        if m == 'popal':
            return ['EDI = POP32(); ESI = POP32(); EBP = POP32(); ESP += 4; EBX = POP32(); EDX = POP32(); ECX = POP32(); EAX = POP32();'], True
        if m == 'leave':
            return ['ESP = EBP; EBP = POP32();'], True
        if m == 'enter':
            if ops[1].imm:
                raise Unsupported('enter level')
            return ['PUSH32(EBP); EBP = ESP; ESP -= %du;' % ops[0].imm], True

        if m in ARITH:
            d, s = ops
            sz = d.size * 8
            sv = self.rd(i, s, sz)
            fnm = ARITH[m]
            if m in ('cmp', 'test'):
                return ['%s(%s, %s, %d);' % (fnm, self.rd(i, d), sv, sz)], True
            if m == 'xor' and d.type == X.X86_OP_REG and s.type == X.X86_OP_REG and d.reg == s.reg:
                return [self.wr(i, d, '0'), 'op_xor(0, 0, %d);' % sz], True
            return ['{ uint32_t r = %s(%s, %s, %d); %s }' % (fnm, self.rd(i, d), sv, sz, self.wr(i, d, 'r'))], True
        if m in ('inc', 'dec', 'neg', 'not'):
            d = ops[0]
            sz = d.size * 8
            return ['{ uint32_t r = op_%s(%s, %d); %s }' % (m, self.rd(i, d), sz, self.wr(i, d, 'r'))], True
        if m in SHIFT:
            d = ops[0]
            sz = d.size * 8
            cnt = '1u' if len(ops) == 1 else self.rd(i, ops[1], 8)
            return ['{ uint32_t r = %s(%s, (%s) & 31u, %d); %s }' % (SHIFT[m], self.rd(i, d), cnt, sz, self.wr(i, d, 'r'))], True
        if m in ('shld', 'shrd'):
            d, s, c = ops
            sz = d.size * 8
            return ['{ uint32_t r = op_%s(%s, %s, (%s) & 31u, %d); %s }' % (m, self.rd(i, d), self.rd(i, s), self.rd(i, c, 8), sz, self.wr(i, d, 'r'))], True
        if m == 'imul':
            if len(ops) == 1:
                return ['op_imul1(%s, %d);' % (self.rd(i, ops[0]), ops[0].size * 8)], True
            if len(ops) == 2:
                d, s = ops
                return ['{ uint32_t r = op_imul(%s, %s, %d); %s }' % (self.rd(i, d), self.rd(i, s), d.size * 8, self.wr(i, d, 'r'))], True
            d, s, c = ops
            return ['{ uint32_t r = op_imul(%s, 0x%xu, %d); %s }' % (self.rd(i, s), c.imm & 0xffffffff, d.size * 8, self.wr(i, d, 'r'))], True
        if m in ('mul', 'div', 'idiv'):
            return ['op_%s1(%s, %d);' % (m, self.rd(i, ops[0]), ops[0].size * 8)], True
        if m == 'cdq':
            return ['EDX = ((int32_t)EAX < 0) ? 0xffffffffu : 0;'], True
        if m == 'cwde':
            return ['EAX = (uint32_t)(int32_t)(int16_t)EAX;'], True
        if m == 'cbw':
            return [self.reg_w('ax', '(uint16_t)(int16_t)(int8_t)EAX')], True
        if m == 'cwd':
            return [self.reg_w('dx', '((int16_t)EAX < 0) ? 0xffffu : 0')], True
        if m.startswith('set') and m[3:] in COND:
            return [self.wr(i, ops[0], '(%s) ? 1u : 0u' % COND[m[3:]])], True
        if m in ('bt', 'bts', 'btr', 'btc'):
            d, s = ops
            sz = d.size * 8
            upd = {'bts': 'v | b', 'btr': 'v & ~b', 'btc': 'v ^ b'}.get(m)
            if d.type == X.X86_OP_MEM and s.type == X.X86_OP_REG:
                L = ['{ int32_t bo = (int32_t)%s; uint32_t p = %s + (uint32_t)((bo >> 5) * 4); uint32_t v = RD32(p); uint32_t b = 1u << (bo & 31);' % (self.rd(i, s), self.addr(i, d)),
                     '  flags_cf((v & b) != 0);']
                L.append(('  WR32(p, %s); }' % upd) if upd else '}')
                return L, True
            L = ['{ uint32_t v = %s; uint32_t b = 1u << ((%s) & %du); flags_cf((v & b) != 0);' % (self.rd(i, d), self.rd(i, s, 8), sz - 1)]
            L.append(('  %s }' % self.wr(i, d, upd)) if upd else '}')
            return L, True
        if m == 'sahf':
            return ['eflags_set((eflags_get() & ~0xd5u) | (%s & 0xd5u));' % self.reg_r('ah')], True
        if m == 'lahf':
            return [self.reg_w('ah', 'eflags_get() & 0xd5u')], True
        if m == 'stc':
            return ['flags_cf(1);'], True
        if m == 'clc':
            return ['flags_cf(0);'], True
        if m == 'cmc':
            return ['flags_cf(!cc_b_raw());'], True
        if m == 'int3':
            return ['rt_fatal("int3 at %08x");' % a], False

        # ---- string instructions
        if m in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
                 'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd'):
            if m == 'movsd' and ops and ops[0].size == 8:
                raise Unsupported('sse movsd')
            sz = {'b': 8, 'w': 16, 'd': 32}[m[-1]]
            kind = m[:-1]
            r = {'': '0', 'rep': '1', 'repe': '1', 'repne': '2'}[rep]
            if kind in ('movs', 'stos', 'lods') and r == '2':
                r = '1'
            return ['str_%s(%d, %s);' % (kind, sz, r)], True

        # ---- branches
        if m == 'jmp':
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                return [self.goto(fn, op.imm & 0xffffffff, a)], False
            if a in self.d.jump_tables:
                L = ['switch (%s) {' % self.rd(i, op)]
                for t in sorted(set(self.d.jump_tables[a])):
                    L.append('  case 0x%xu: %s' % (t, self.goto(fn, t, a)))
                L.append('  default: rt_jump(%s); return;' % self.rd(i, op))
                L.append('}')
                return L, False
            return ['rt_jump(%s); return;' % self.rd(i, op)], False
        if m[0] == 'j' and m[1:] in COND:
            return ['if (%s) %s' % (COND[m[1:]], self.goto(fn, ops[0].imm & 0xffffffff, a))], True
        if m == 'jecxz':
            return ['if (ECX == 0) %s' % self.goto(fn, ops[0].imm & 0xffffffff, a)], True
        if m in ('loop', 'loope', 'loopne'):
            c = {'loop': '', 'loope': ' && cc_z()', 'loopne': ' && !cc_z()'}[m]
            return ['if (--ECX != 0%s) %s' % (c, self.goto(fn, ops[0].imm & 0xffffffff, a))], True
        if m == 'call':
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm & 0xffffffff
                if self.is_entry(t):
                    return ['CALL(f_%08x, 0x%xu);' % (t, nxt)], True
                return ['rt_call(0x%xu, 0x%xu);' % (t, nxt)], True
            return ['{ uint32_t t = %s; rt_call(t, 0x%xu); }' % (self.rd(i, op), nxt)], True
        if m == 'ret':
            n = ops[0].imm if ops else 0
            return ['ESP += %du; return;' % (4 + n)], False

        if m.startswith('f'):
            return self.fpu(i, m, ops), True
        raise Unsupported('mnemonic')

    # ------------------------------------------------------------ x87
    def fpu(self, i, m, ops):
        def mem(op):
            return self.addr(i, op), op.size
        def st(op):
            return op.reg - X.X86_REG_ST0
        if m in ('fld', 'fild'):
            op = ops[0]
            if op.type == X.X86_OP_REG:
                return ['fpu_push(ST(%d));' % st(op)]
            e, s = mem(op)
            k = {('fld', 4): 'f32', ('fld', 8): 'f64', ('fld', 10): 'f80',
                 ('fild', 2): 'i16', ('fild', 4): 'i32', ('fild', 8): 'i64'}[(m, s)]
            return ['fpu_push(ld_%s(%s));' % (k, e)]
        if m in ('fst', 'fstp', 'fist', 'fistp'):
            op = ops[0]
            pop = m.endswith('p')
            if op.type == X.X86_OP_REG:
                L = ['ST(%d) = ST(0);' % st(op)]
            else:
                e, s = mem(op)
                k = ('i%d' if m.startswith('fist') else 'f%d') % (s * 8)
                L = ['st_%s(%s, ST(0));' % (k, e)]
            if pop:
                L.append('fpu_pop();')
            return L
        arith = {'fadd': '+', 'fmul': '*', 'fsub': '-', 'fdiv': '/', 'fsubr': 'r-', 'fdivr': 'r/'}
        base = m[:-1] if m.endswith('p') and m[:-1] in arith else m
        if base in arith or m in ('fiadd', 'fisub', 'fimul', 'fidiv', 'fisubr', 'fidivr'):
            pop = m.endswith('p') and base in arith
            if m.startswith('fi'):
                opn = {'fiadd': '+', 'fisub': '-', 'fimul': '*', 'fidiv': '/', 'fisubr': 'r-', 'fidivr': 'r/'}[m]
            else:
                opn = arith[base]
            def comb(x, y):
                if opn[0] == 'r':
                    return '(%s) %s (%s)' % (y, opn[1], x)
                return '(%s) %s (%s)' % (x, opn, y)
            if m.startswith('fi'):
                e, s = mem(ops[0])
                return ['ST(0) = %s;' % comb('ST(0)', 'ld_i%d(%s)' % (s * 8, e))]
            if len(ops) == 1 and ops[0].type == X.X86_OP_MEM:
                e, s = mem(ops[0])
                return ['ST(0) = %s;' % comb('ST(0)', 'ld_f%d(%s)' % (s * 8, e))]
            if len(ops) == 0:
                dst, src = 1, 0
            elif len(ops) == 1:
                dst, src = 0, st(ops[0])
            else:
                dst, src = st(ops[0]), st(ops[1])
            L = ['ST(%d) = %s;' % (dst, comb('ST(%d)' % dst, 'ST(%d)' % src))]
            if pop:
                L.append('fpu_pop();')
            return L
        if m in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp', 'ficom', 'ficomp'):
            if ops and ops[0].type == X.X86_OP_MEM:
                e, s = mem(ops[0])
                other = ('ld_i%d(%s)' if m.startswith('fi') else 'ld_f%d(%s)') % (s * 8, e)
            elif ops:
                other = 'ST(%d)' % st(ops[0])
            else:
                other = 'ST(1)'
            L = ['fpu_compare(ST(0), %s);' % other]
            if m.endswith('pp'):
                L += ['fpu_pop();', 'fpu_pop();']
            elif m.endswith('p'):
                L.append('fpu_pop();')
            return L
        if m == 'fxch':
            r = st(ops[0]) if ops else 1
            return ['{ double t = ST(0); ST(0) = ST(%d); ST(%d) = t; }' % (r, r)]
        if m == 'fnstsw':
            if ops[0].type == X.X86_OP_REG:
                return [self.reg_w('ax', 'fpu_sw()')]
            return ['WR16(%s, fpu_sw());' % self.addr(i, ops[0])]
        if m == 'fnstcw':
            return ['WR16(%s, FPU_CW);' % self.addr(i, ops[0])]
        if m == 'fldcw':
            return ['FPU_CW = RD16(%s);' % self.addr(i, ops[0])]
        simple = {
            'fldz': 'fpu_push(0.0);', 'fld1': 'fpu_push(1.0);', 'fldpi': 'fpu_push(3.14159265358979323846);',
            'fldl2e': 'fpu_push(1.44269504088896340736);', 'fldln2': 'fpu_push(0.693147180559945309417);',
            'fldlg2': 'fpu_push(0.301029995663981195);', 'fldl2t': 'fpu_push(3.32192809488736234787);',
            'fchs': 'ST(0) = -ST(0);', 'fabs': 'ST(0) = fabs(ST(0));', 'fsqrt': 'ST(0) = sqrt(ST(0));',
            'fsin': 'ST(0) = sin(ST(0)); FPU_C2 = 0;', 'fcos': 'ST(0) = cos(ST(0)); FPU_C2 = 0;',
            'fsincos': '{ double v = ST(0); ST(0) = sin(v); fpu_push(cos(v)); FPU_C2 = 0; }',
            'fptan': 'ST(0) = tan(ST(0)); fpu_push(1.0); FPU_C2 = 0;',
            'fpatan': 'ST(1) = atan2(ST(1), ST(0)); fpu_pop();',
            'frndint': 'ST(0) = fpu_round(ST(0));', 'fscale': 'ST(0) = ldexp(ST(0), (int)trunc(ST(1)));',
            'f2xm1': 'ST(0) = exp2(ST(0)) - 1.0;', 'fyl2x': 'ST(1) = ST(1) * log2(ST(0)); fpu_pop();',
            'fprem': 'ST(0) = fmod(ST(0), ST(1)); FPU_C2 = 0;', 'ftst': 'fpu_compare(ST(0), 0.0);',
            'fxam': 'fpu_xam();', 'fninit': 'fpu_init();', 'finit': 'fpu_init();',
        }
        if m in simple:
            return [simple[m]]
        raise Unsupported('fpu')


# ------------------------------------------------------------ patch points
# Instructions replaced by C. Every patch keeps the original behaviour when the feature is off,
# so the generated code still runs the retail game unchanged.
#
# English dialogue (runtime/vwf.c), ADV text interpreter f_00478cf0 - the same hook points as
# the PS1 translation's ADV patch (src/asm/common/shared_patches.inc there):
#   * byte classification: two-byte glyph codes are 80-87 only, so 88-E6 are English characters
#   * the glyph call goes to the variable-width engine, which also activates the cells it
#     touched and owns the cell position (state+0xA), so the interpreter's own activate call
#     and pos++ are skipped
def dialogue_patches(interp, classify, single, text_emit, name_emit):
    """The English dialogue hooks for one copy of the MsgStep text interpreter. Each copy has the
    same shape: `cmp bx, 0x7f` (classify) / `jbe single`; glyph call (5 bytes) followed by
    `mov cx,[esi+0xa]; mov edx,ebx` (5 bytes), the activate call (5 bytes), one 4-byte
    instruction and `inc word [esi+0xa]`, for both the text and the name-insertion path."""
    p = {
        classify: ['vwf_two_byte = !((uint16_t)EBX <= 0x7f || (vwf_on[%s] && (uint16_t)EBX >= 0x88));' % interp,
                   'if (!vwf_two_byte) goto L_%x;' % single],
        classify + 4: [],
    }
    glyph = None
    for emit, name in ((text_emit, 0), (name_emit, 1)):
        act, inc = emit + 0x0b, emit + 0x14
        p[emit] = (['vwf_two_byte = 0;'] if name else []) + [
            'if (vwf_on[%s]) { vwf_emit(%s, %d); ESP += 4; } else CALL(f_%08x, 0x%xu);' % (interp, interp, name, GLYPH[interp], emit + 5)]
        p[act] = ['if (!vwf_on[%s]) CALL(f_%08x, 0x%xu);' % (interp, ACTIVATE[interp], act + 5)]
        p[inc] = ['if (!vwf_on[%s]) WR16(ESI + 0xau, op_inc(RD16(ESI + 0xau), 16));' % interp]
    return p


GLYPH = {'VWF_ADV': 0x479670, 'VWF_B': 0x421310, 'VWF_C': 0x4e2150}
ACTIVATE = {'VWF_ADV': 0x479610, 'VWF_B': 0x4212b0, 'VWF_C': 0x4e20f0}
PATCHES = {}
PATCHES.update(dialogue_patches('VWF_ADV', 0x479522, 0x479535, 0x47954c, 0x479463))
PATCHES.update(dialogue_patches('VWF_B', 0x4211cb, 0x4211de, 0x4211f5, 0x42110c))
PATCHES.update(dialogue_patches('VWF_C', 0x4e200b, 0x4e201e, 0x4e2035, 0x4e1f4c))

# The dungeon field script (runtime/vwf_field.c). `test al, 0x80` in the interpreter (0x42d8a2) and
# its string printers (0x42dd17, 0x42ddcc) decides between a one-byte and a two-byte glyph: with
# English on, only 80-87 lead a two-byte glyph. FieldMsgPutGlyph (0x42da00) goes to the English
# engine, and the interpreter's read at the top of its loop (0x42cdd3) checks for spilled boxes.
FIELD_LEAD = ['{ uint8_t al = (uint8_t)EAX;',
              '  vwf_field_two = al >= 0x80 && !(vwf_field_on && al >= 0x88);',
              '  op_and(vwf_field_two ? 0x80u : 0u, 0x80u, 8); }']
for a in (0x42d8a2, 0x42dd17, 0x42ddcc):
    PATCHES[a] = FIELD_LEAD
PATCHES[0x42da00] = ['if (vwf_field_on) { vwf_field_put(); ESP += 4u; return; }',
                     'R[0] = (uint32_t)(RD32((uint32_t)(0x5e46e8u)));']
PATCHES[0x42cdd3] = ['if (vwf_field_on) ESI = dng_spill(ESI);',
                     'R[0] = (R[0] & 0xffffff00u) | (uint8_t)(RD8((uint32_t)(R[6])));']

# MapDrawName: long dungeon map titles (runtime/vwf_field.c; the PS1 patch's dng/map_title.asm)
PATCHES[0x4079e0] = ['if (vwf_field_on && vwf_on[VWF_B] && map_title()) { ESP += 4u; return; }',
                     'R[0] = (uint32_t)(RD32((uint32_t)(0x5daaa8u)));']

# The battle text system's glyph function (PS1 BTLP 0x8007C754; runtime/vwf_btl.c).
PATCHES[0x52fbb0] = ['if (vwf_btl_on) { vwf_btl_glyph(); ESP += 4u; return; }',
                     '{ uint32_t t = R[1]; PUSH32(t); }']
# the interpreter's read pointer: a matched battle string -> its English (tools/btl_map.py)
PATCHES[0x52fd6d] = ['if (vwf_btl_on) WR32(ESI + 8u, btl_text(RD32(ESI + 8u)));',
                     'R[0] = (uint32_t)(RD32((uint32_t)(R[6] + 0x8u)));']
# and its 16 px string renderer (PS1 BTLP 0x8007BF44: command windows)
PATCHES[0x52f2c0] = ['if (vwf_btl_on) { vwf_btl_str16(); ESP += 4u; return; }',
                     '{ uint32_t t = R[1]; PUSH32(t); }']


def main():
    exe, out = sys.argv[1], sys.argv[2]
    img = Image(exe)
    disc = Discovery(img, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'extra_entries.txt')).run()
    gen = Gen(img, disc)
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.startswith('code_') and f.endswith('.c'):
            os.remove(os.path.join(out, f))
    funcs = sorted(disc.entries)
    hdr = '#include "cpu.h"\n#include "funcs.h"\n'
    CHUNK = 400
    for c in range(0, len(funcs), CHUNK):
        with open(os.path.join(out, 'code_%03d.c' % (c // CHUNK)), 'w') as f:
            f.write(hdr)
            for e in funcs[c:c + CHUNK]:
                f.write('\n'.join(gen.emit_fn(e)))
                f.write('\n\n')
    with open(os.path.join(out, 'funcs.h'), 'w') as f:
        f.write('#pragma once\n')
        for e in funcs:
            f.write('void f_%08x(void);\n' % e)
    with open(os.path.join(out, 'table.c'), 'w') as f:
        f.write(hdr)
        f.write('const struct guest_fn GUEST_FUNCS[] = {\n')
        for e in funcs:
            f.write('  {0x%xu, f_%08x},\n' % (e, e))
        f.write('};\nconst int GUEST_NFUNCS = %d;\n' % len(funcs))
        f.write('const uint32_t GUEST_ENTRY = 0x%xu;\n' % img.entry)
        f.write('const struct guest_import GUEST_IMPORTS[] = {\n')
        for a, (dll, name) in sorted(img.imports.items()):
            f.write('  {0x%xu, "%s", "%s"},\n' % (a, dll, name))
        f.write('};\nconst int GUEST_NIMPORTS = %d;\n' % len(img.imports))
    with open(os.path.join(out, 'image_info.h'), 'w') as f:
        data = open(exe, 'rb').read()
        f.write('#pragma once\n#define IMAGE_BASE 0x%xu\n#define IMAGE_SIZE 0x%xu\n' % (img.base, img.size))
        lo = min(b for b, e in img.code)
        hi = max(e for b, e in img.code)
        f.write('#define TEXT_LO 0x%xu\n#define TEXT_HI 0x%xu\n' % (lo, hi))
        f.write('#define EXE_SIZE %du\n#define EXE_CRC32 0x%08xu\n' % (len(data), zlib.crc32(data)))
    print('functions %d, jump tables %d' % (len(funcs), len(disc.jump_tables)))
    print('unsupported:', gen.unimpl.most_common(30))


if __name__ == '__main__':
    main()
