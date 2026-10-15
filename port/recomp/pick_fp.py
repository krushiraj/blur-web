import sys, random
exec(open('lift.py').read().split("if __name__")[0])
random.seed(7); fp=[]
for e in sorted(FUNCS):
    if FUNCS[e][0] > 400: continue
    try: L=FnLifter(e)
    except Unsupported: continue
    ok=True; hasfp=False
    for a in L.order:
        i=L.ins[a]; m=i.mnemonic
        if m in('call','int3') or (m=='jmp' and i.operands[0].type!=X.X86_OP_IMM) or m.startswith('lock'): ok=False;break
        if m.startswith('f') or any(o.type==X.X86_OP_REG and X.X86_REG_XMM0<=o.reg<=X.X86_REG_XMM7 for o in i.operands): hasfp=True
        try: L.insn(i)
        except Unsupported: ok=False;break
    if ok and hasfp: fp.append(e)
random.shuffle(fp); fp=sorted(fp[:500])
open('oracle_funcs_fp.txt','w').write('\n'.join('%08x'%e for e in fp)+'\n'); print(len(fp))
