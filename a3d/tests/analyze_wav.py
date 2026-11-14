import numpy as np, struct, sys
def readwav(p):
    d=open(p,'rb').read(); off=12; fmt=None
    while off+8<=len(d):
        cid=d[off:off+4]; ln=struct.unpack('<I',d[off+4:off+8])[0]; body=d[off+8:off+8+ln]
        if cid==b'fmt ': tag,ch,sr=struct.unpack('<HHI',body[:8]); bits=struct.unpack('<H',body[14:16])[0]
        if cid==b'data': data=body
        off+=8+ln+(ln&1)
    x=np.frombuffer(data,dtype=np.float32 if bits==32 else np.int16).reshape(-1,ch).astype(float)
    if bits==16: x/=32768
    return x,sr
x,sr=readwav(sys.argv[1] if len(sys.argv)>1 else 'run/out.wav')
e=np.abs(x).sum(1); on=int(np.argmax(e>1e-4)); print('len',len(x)/sr,'onset',on/sr)
from numpy.fft import rfft
blk=int(0.25*sr)
for i in range(on, len(x)-blk, blk):
    b=x[i:i+blk]; L,R=b[:,0],b[:,1]
    rl,rr=np.sqrt((L**2).mean()),np.sqrt((R**2).mean())
    if rl+rr<1e-6: print(f"{(i-on)/sr:5.2f} silent"); continue
    F=np.abs(rfft(L+R))**2; f=np.fft.rfftfreq(len(L),1/sr)
    hf=F[(f>4000)&(f<10000)].sum()/F[(f>200)&(f<2000)].sum()
    print(f"{(i-on)/sr:5.2f} L {20*np.log10(rl+1e-9):6.1f} R {20*np.log10(rr+1e-9):6.1f} bal {20*np.log10((rr+1e-9)/(rl+1e-9)):6.1f} hf/lf {10*np.log10(hf+1e-12):6.1f}")
