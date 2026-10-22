import sys, bisect
nm=[]
for l in open(sys.argv[2]):
    p=l.split()
    if len(p)==3 and p[1] in 'TtWw': nm.append((int(p[0],16),p[2]))
nm.sort(); addrs=[a for a,_ in nm]
rows=[]
for l in open(sys.argv[1]):
    pc,c=l.split(); rows.append((int(c),int(pc,16)))
rows.sort(reverse=True); tot=sum(c for c,_ in rows)
byfn={}
for c,pc in rows:
    i=bisect.bisect_right(addrs,pc)-1
    fn=nm[i][1] if i>=0 else '?'
    if pc==1: fn='(waiti idle)'
    byfn[fn]=byfn.get(fn,0)+c
for fn,c in sorted(byfn.items(),key=lambda x:-x[1])[:int(sys.argv[3]) if len(sys.argv)>3 else 15]: print(f"{100*c/tot:5.1f}% {fn}")
print('top pcs:', ' '.join(f"{pc:08x}:{c}" for c,pc in rows[:8]))
