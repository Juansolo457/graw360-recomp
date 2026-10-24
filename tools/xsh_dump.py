# Decode the renderer's shader storage (port/cache/shaders/shareable/555307D5.xsh): every guest
# shader the stock Xenos plugin has translated, as raw Xenos microcode. No game run is needed
# (--dump_shaders=<dir> is the in-game way; it needs the '='). The game must be closed: the file
# is locked while it runs.
#   python -I tools/xsh_dump.py port/cache/shaders/shareable/555307D5.xsh            # Bink-like pixel shaders
#   python -I tools/xsh_dump.py <file.xsh> --hash E4F08314542740C2                   # one shader
# Format: 8-byte header ('XESH', version), then per shader: u64 ucode hash, u32 dword count
# (bit 31 = pixel shader), then the microcode as big-endian dwords. Decodes control flow
# (exec/alloc), texture fetches and the vector half of ALU instructions; scalar halves are
# raw. Layouts: ReXGlue include/rex/graphics/format/ucode.h.
import struct, sys
VOP = {0:'add',1:'mul',2:'max',3:'min',4:'seq',5:'sgt',6:'sge',7:'sne',8:'frc',9:'trunc',10:'floor',
       11:'mad',12:'cndeq',13:'cndge',14:'cndgt',15:'dp4',16:'dp3',17:'dp2add',18:'cube',19:'max4',
       20:'setp_eq_push',21:'setp_ne_push',22:'setp_gt_push',23:'setp_ge_push',24:'kill_eq',25:'kill_gt',
       26:'kill_ge',27:'kill_ne',28:'dst',29:'maxa'}
def bits(v,lo,n): return (v>>lo)&((1<<n)-1)
def swz(s, n=4):  # ALU source swizzle: 2 bits per component, relative to component index
    return ''.join('xyzw'[(bits(s,2*i,2)+i)&3] for i in range(4))
def load(path):
    d=open(path,'rb').read()
    magic,ver=struct.unpack_from('<II',d,0); off=8; out=[]
    while off+12<=len(d):
        h,cw=struct.unpack_from('<QI',d,off); n=cw&0x7FFFFFFF; t=cw>>31; off+=12
        words=list(struct.unpack_from('>%dI'%n,d,off)); off+=4*n
        out.append((h,t,words))
    return out
def cf(words):
    # control flow is at the start, 3 dwords per 2 instructions, until the first exec's address
    insts=[]; i=0; limit=len(words)
    while i*3 < limit and i*3+2 < len(words):
        a,b,c=words[i*3:i*3+3]
        for d0,d1 in ((a, b&0xFFFF), ((b>>16)|((c<<16)&0xFFFFFFFF), c>>16)):
            op=bits(d1,12,4)
            insts.append((op,d0,d1))
            if op in (1,2,3,4,5,6,7,8,9,10,11,12,13,14):
                pass
        i+=1
        # stop at end
        if any(o in (2,4,7,14) for o,_,_ in insts[-2:]):  # exec_end variants
            break
    return insts
def decode(words):
    lines=[]
    for op,d0,d1 in cf(words):
        if op not in (1,2,3,4,6,7,13,14): 
            lines.append(f'cf op {op}'); continue
        addr=bits(d0,0,12); cnt=bits(d0,12,3); seq=bits(d0,16,12)
        lines.append(f'exec op{op} addr {addr} count {cnt}')
        for k in range(cnt):
            w=words[(addr+k)*3:(addr+k)*3+3]
            if (seq>>(2*k))&1:
                w0,w1,w2=w; fop=bits(w0,0,5)
                if fop==1:
                    dst=bits(w0,12,6); src=bits(w0,5,6); const=bits(w0,20,5); dsw=bits(w1,0,12)
                    comp=''.join('xyzw01?_'[bits(dsw,3*i,3)] for i in range(4))
                    lines.append(f'  tfetch r{dst}.{comp} = tf{const}[r{src}] dim {bits(w2,19,2)}')
                else: lines.append(f'  fetch op {fop} {[hex(x) for x in w]}')
            else:
                w0,w1,w2=w
                vd=bits(w0,0,6); exp=bits(w0,15,1); vm=bits(w0,16,4); sop=bits(w0,26,6)
                s3s,s2s,s1s=bits(w1,0,8),bits(w1,8,8),bits(w1,16,8)
                n3,n2,n1=bits(w1,24,1),bits(w1,25,1),bits(w1,26,1)
                r3,r2,r1=bits(w2,0,8),bits(w2,8,8),bits(w2,16,8)
                vop=bits(w2,24,5); sel3,sel2,sel1=bits(w2,29,1),bits(w2,30,1),bits(w2,31,1)
                def src(r,sel,s,neg): return ('-' if neg else '')+(f'r{r&63}' if sel else f'c{r}')+'.'+swz(s)
                mask=''.join('xyzw'[i] for i in range(4) if vm>>i&1) or '_'
                lines.append(f"  {VOP.get(vop,vop)} {'o' if exp else 'r'}{vd}.{mask} = {src(r1,sel1,s1s,n1)}, {src(r2,sel2,s2s,n2)}, {src(r3,sel3,s3s,n3)}   (scalar op {sop})")
    return lines
if __name__=='__main__':
    sh=load(sys.argv[1]); print(len(sh),'shaders')
    want=sys.argv[sys.argv.index('--hash')+1].upper() if '--hash' in sys.argv else None
    for h,t,w in sh:
        if want:
            if f'{h:016X}'!=want: continue
            print(f'== {h:016X} {"pixel" if t else "vertex"}, {len(w)} dwords')
            print('\n'.join(decode(w)))
            continue
        if t!=1: continue
        txt=decode(w)
        nf=sum('tfetch' in l for l in txt); nd=sum(' dp4 ' in l for l in txt)
        if nf in (3,4) and nd>=3 and len(w)<120:
            print(f'== {h:016X} pixel, {len(w)} dwords')
            print('\n'.join(txt))
