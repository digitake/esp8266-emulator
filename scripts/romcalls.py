import re, struct, sys, collections
elf_path, dis_path, nm_path = sys.argv[1:4]
data = open(elf_path,'rb').read()
e_shoff, = struct.unpack_from('<I', data, 0x20); e_shentsize, e_shnum = struct.unpack_from('<HH', data, 0x2e)
secs=[]
for i in range(e_shnum):
    nm, typ, flags, addr, off, size = struct.unpack_from('<IIIIII', data, e_shoff+i*e_shentsize)
    if addr and typ==1: secs.append((addr, off, size))
def rd32(a):
    for addr, off, size in secs:
        if addr <= a < addr+size-3: return struct.unpack_from('<I', data, off + a-addr)[0]
    return None
syms={}
for l in open(nm_path):
    p=l.split()
    if len(p)==3: syms.setdefault(int(p[0],16), p[2])
lines=open(dis_path).read().splitlines()
regval={}
calls=collections.defaultdict(list)
func='?'
for l in lines:
    m=re.match(r'^([0-9a-f]{8}) <(.*)>:', l)
    if m: func=m.group(2); regval={}; continue
    m=re.match(r'^([0-9a-f]{8}):\s+[0-9a-f]+\s+(\S+)\s*(.*)$', l)
    if not m: continue
    op, args = m.group(2), m.group(3)
    if op=='l32r':
        r, tgt = args.split(',')[0], int(args.split(',')[1].split()[0],16)
        regval[r]=rd32(tgt)
    elif op in ('movi','movi.n'):
        p=args.split(',');
        try: regval[p[0].strip()]=int(p[1].strip(),0)
        except: pass
    elif op=='callx0':
        r=args.strip(); v=regval.get(r)
        if v is not None and 0x40000000<=v<0x40010000:
            calls[v].append((func, regval.get('a2'), regval.get('a3')))
        regval={}
    elif op=='call0':
        v=int(args.split()[0],16)
        if 0x40000000<=v<0x40010000: calls[v].append((func, regval.get('a2'), regval.get('a3')))
        regval={}
    elif op in ('ret','ret.n','j','jx'): regval={}
for v in sorted(calls):
    c=calls[v]
    print(f"{v:08x} {syms.get(v,'?'):28s} n={len(c):3d}  " + '; '.join(f"{f}(a2={a2 if a2 is None else hex(a2)})" for f,a2,a3 in c[:4]))
