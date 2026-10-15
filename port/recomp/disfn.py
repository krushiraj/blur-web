import sys
exec(open('lift.py').read().split("if __name__")[0])
L=FnLifter(int(sys.argv[1],16))
for a in L.order: i=L.ins[a]; print('%08x  %s %s'%(a,i.mnemonic,i.op_str))
