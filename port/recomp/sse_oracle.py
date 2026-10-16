"""Instruction-level differential test of the SSE/SSE2 lifting: one tiny function per instruction form, lifted and compared against Unicorn with oracle.c.
usage: /tmp/blur-venv/bin/python sse_oracle.py   (writes sse_work/, builds sse_work/oracle_sse; run it with: ./sse_work/oracle_sse sse_work/img.bin 300)"""
import os, subprocess, sys
H = os.path.dirname(os.path.abspath(__file__)); W = os.path.join(H, 'sse_work'); os.makedirs(W, exist_ok=True)
LLVM = '/opt/homebrew/opt/llvm/bin/'
ps = ['addps', 'subps', 'mulps', 'divps', 'minps', 'maxps', 'andps', 'andnps', 'orps', 'xorps', 'unpcklps', 'unpckhps', 'movlhps', 'movhlps', 'cvtdq2ps', 'cvtps2dq', 'cvttps2dq', 'cvtps2pd', 'cvtpd2ps',
      'sqrtps', 'rcpps', 'rsqrtps', 'movaps', 'movups', 'addss', 'subss', 'mulss', 'divss', 'minss', 'maxss', 'sqrtss', 'movss', 'cvtss2sd', 'cvtsd2ss',
      'addpd', 'subpd', 'mulpd', 'divpd', 'minpd', 'maxpd', 'andpd', 'orpd', 'xorpd', 'addsd', 'subsd', 'mulsd', 'divsd', 'minsd', 'maxsd', 'sqrtsd', 'movsd', 'movapd', 'movupd', 'unpcklpd', 'unpckhpd',
      'paddd', 'paddw', 'paddb', 'paddq', 'psubd', 'psubw', 'psubb', 'psubq', 'pand', 'pandn', 'por', 'pxor', 'pcmpeqd', 'pcmpeqw', 'pcmpeqb', 'pcmpgtd', 'pcmpgtw', 'pcmpgtb', 'punpcklbw', 'punpcklwd', 'punpckldq', 'punpcklqdq',
      'punpckhbw', 'punpckhwd', 'punpckhdq', 'punpckhqdq', 'packssdw', 'packsswb', 'packuswb', 'pmullw', 'pmulhw', 'pmulhuw', 'pmuludq', 'pmaddwd', 'pminub', 'pmaxub', 'pminsw', 'pmaxsw', 'paddusb', 'paddusw', 'paddsb', 'paddsw',
      'psubusb', 'psubusw', 'psubsb', 'psubsw', 'pavgb', 'pavgw', 'psadbw', 'movdqa', 'movdqu', 'psllw', 'pslld', 'psllq', 'psrlw', 'psrld', 'psrlq', 'psraw', 'psrad', 'movq', 'cvtdq2pd', 'cvttpd2dq', 'cvtpd2dq']
imm = ['shufps', 'shufpd', 'pshufd', 'pshuflw', 'pshufhw']
cmp_ = ['cmpps', 'cmpss', 'cmppd', 'cmpsd']
shifts = ['pslld', 'psrld', 'psrad', 'psllw', 'psrlw', 'psraw', 'psllq', 'psrlq', 'pslldq', 'psrldq']
com = ['comiss', 'ucomiss', 'comisd', 'ucomisd']
sn = []
def add(name, body): sn.append((name, body))
for o in ps:
    add(f'{o}_rr', [f'{o} xmm0, xmm1']); add(f'{o}_rr35', [f'{o} xmm3, xmm5'])
    if o not in ('movlhps', 'movhlps'): add(f'{o}_rm', [f'{o} xmm0, [esi+0x20]'])
for o in imm:
    for i in (0x00, 0x1b, 0xe4, 0x4e, 0xb1, 0xff, 0x93):
        add(f'{o}_rr_{i:02x}', [f'{o} xmm0, xmm1, {i}']); add(f'{o}_rm_{i:02x}', [f'{o} xmm0, [esi+0x20], {i}'])
for o in cmp_:
    for i in range(8): add(f'{o}_{i}', [f'{o} xmm0, xmm1, {i}'])
for o in shifts:
    for i in (1, 3, 7, 15, 31, 33, 63, 64): add(f'{o}_i{i}', [f'{o} xmm0, {i}'])
for o in com: add(f'{o}_f', [f'{o} xmm0, xmm1', 'setp bl', 'setb cl', 'sete dl'])
add('movmskps', ['movmskps eax, xmm1']); add('movmskpd', ['movmskpd eax, xmm1']); add('pmovmskb', ['pmovmskb eax, xmm1'])
add('movd_to', ['movd xmm0, ecx']); add('movd_from', ['movd eax, xmm1']); add('movd_mem', ['movd xmm0, dword ptr [esi+0x24]'])
add('movq_mem', ['movq xmm0, qword ptr [esi+0x28]']); add('movq_store', ['movq qword ptr [esi+0x40], xmm1'])
add('cvtsi2ss', ['cvtsi2ss xmm0, ecx']); add('cvtsi2sd', ['cvtsi2sd xmm0, ecx']); add('cvtss2si', ['cvtss2si eax, xmm1']); add('cvttss2si', ['cvttss2si eax, xmm1']); add('cvttsd2si', ['cvttsd2si eax, xmm1']); add('cvtsd2si', ['cvtsd2si eax, xmm1'])
add('cvtss2si_m', ['cvttss2si eax, dword ptr [esi+0x20]'])
for st in ('movaps', 'movups', 'movdqa', 'movdqu', 'movapd', 'movupd'): add(f'{st}_store', [f'{st} xmmword ptr [esi+0x40], xmm1'])
add('movss_store', ['movss dword ptr [esi+0x44], xmm1']); add('movsd_store', ['movsd qword ptr [esi+0x48], xmm1'])
add('movlps_load', ['movlps xmm0, qword ptr [esi+0x20]']); add('movhps_load', ['movhps xmm0, qword ptr [esi+0x20]']); add('movlps_store', ['movlps qword ptr [esi+0x40], xmm1']); add('movhps_store', ['movhps qword ptr [esi+0x40], xmm1'])
add('movlpd_load', ['movlpd xmm0, qword ptr [esi+0x20]']); add('movhpd_load', ['movhpd xmm0, qword ptr [esi+0x20]']); add('movhpd_store', ['movhpd qword ptr [esi+0x40], xmm1'])
add('movss_load', ['movss xmm0, dword ptr [esi+0x24]']); add('movsd_load', ['movsd xmm0, qword ptr [esi+0x28]'])
add('movups_unal', ['movups xmm0, xmmword ptr [esi+0x23]']); add('movdqu_unal', ['movdqu xmm0, xmmword ptr [esi+0x27]'])
add('movntps', ['movntps xmmword ptr [esi+0x40], xmm1']); add('movntdq', ['movntdq xmmword ptr [esi+0x40], xmm1'])
add('pextrw', ['pextrw eax, xmm1, 3']); add('pinsrw', ['pinsrw xmm0, ecx, 5']); add('pinsrw_m', ['pinsrw xmm0, word ptr [esi+0x22], 2'])
lines = ['.intel_syntax noprefix', '.text']
for name, body in sn:
    lines += ['.balign 128', f'sn_{name}:', 'mov esi, 0x30001000'] + body + ['ret']
open(os.path.join(W, 'sn.s'), 'w').write('\n'.join(lines) + '\n')
subprocess.check_call([LLVM + 'llvm-mc', '-triple=i386-unknown-linux-gnu', '-filetype=obj', os.path.join(W, 'sn.s'), '-o', os.path.join(W, 'sn.o')])
subprocess.check_call([LLVM + 'llvm-objcopy', '-O', 'binary', '--only-section=.text', os.path.join(W, 'sn.o'), os.path.join(W, 'sn.bin')])
text = open(os.path.join(W, 'sn.bin'), 'rb').read()
img = bytearray(0x1000) + text; img += bytes((-len(img)) % 0x1000)
open(os.path.join(W, 'img.bin'), 'wb').write(img)
addrs = [0x401000 + 128 * i for i in range(len(sn))]
names = {a: n for a, (n, _) in zip(addrs, sn)}
env = dict(os.environ, LIFT_IMG=os.path.join(W, 'img.bin'))
gen = os.path.join(W, 'gen'); os.makedirs(gen, exist_ok=True)
ok = []
for a in addrs:   # lift each separately so one unsupported form does not hide the rest
    out = os.path.join(gen, f'g_{a:08x}.c')
    r = subprocess.run([sys.executable, os.path.join(H, 'lift.py'), 'gen', out, f'{a:x}'], env=env, capture_output=True, text=True)
    if r.returncode == 0 and 'lifted [' in r.stdout and os.path.exists(out) and os.path.getsize(out) > 200: ok.append(a)
    else: print('NOT LIFTED', names[a], (r.stderr or r.stdout).strip().splitlines()[-1:])
tab = ['#include "rt.h"', 'typedef struct { uint32_t addr; GuestFn fn; } Ent;'] + [f'void f_{a:08x}(Cpu *c);' for a in ok] + ['Ent table[] = {'] + [f'  {{0x{a:x}, f_{a:08x}}},' for a in ok] + ['};', f'int ntable = {len(ok)};']
open(os.path.join(gen, 'table.c'), 'w').write('\n'.join(tab) + '\n')
srcs = [os.path.join(gen, f) for f in os.listdir(gen) if f.endswith('.c')]
subprocess.check_call(['clang', '-O1', '-w', '-ffp-contract=off', '-I', H, '-I/opt/homebrew/include', os.path.join(H, 'oracle.c'), os.path.join(H, 'rt_vec.c')] + srcs + ['-L/opt/homebrew/lib', '-lunicorn', '-lm', '-o', os.path.join(W, 'oracle_sse')])
open(os.path.join(W, 'names.txt'), 'w').write('\n'.join(f'{a:08x} {names[a]}' for a in addrs))
print('built', len(ok), 'of', len(addrs))
