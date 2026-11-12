"""PRISM r3 Chief: the armour material's emissive image, the base map's lamp texels (the texels CE's light-shield
faces sample, which r3 draws as armour), from a tag_export.py npz of CE's model.

    python -I art/prism_r3_emissive.py <ce npz> <base.png> <out.png>
Local only (CE pixels).
"""
import numpy as np, cv2, sys
from PIL import Image
d=np.load(sys.argv[1]); base=np.asarray(Image.open(sys.argv[2]).convert('RGB')).astype(np.float32)/255; out=sys.argv[3]
n=base.shape[0]; sh=[str(x) for x in d['shaders']]; print(sh)
lamp=[i for i,s in enumerate(sh) if 'light shield' in s]
tri=d['tri'][np.isin(d['tri_shader'],lamp)]; uv=d['uv']
best=None
for flip in (False,):  # (the npz's uvs are the picture's: v down)
    m=np.zeros((n,n),np.uint8)
    for t in tri:
        p=uv[t].copy()
        if flip: p[:,1]=1-p[:,1]
        cv2.fillPoly(m,[np.round(p*n).astype(np.int32)],1)
    lum=(base.mean(2)*m).sum()/max(m.sum(),1)
    print('flip',flip,'texels',int(m.sum()),'mean lum',round(float(lum),3))
    if best is None or lum>best[0]: best=(lum,m)
m=cv2.dilate(best[1],np.ones((9,9),np.uint8))
em=base*m[...,None]
# (the glow's colour kept, its dark surround dropped: only texels brighter than 0.25)
em=np.where(em.mean(2,keepdims=True)>0.25, em, 0)
Image.fromarray((em*255+0.5).astype(np.uint8)).save(out); print('EMISSIVE_OK', int((em.mean(2)>0).sum()))
