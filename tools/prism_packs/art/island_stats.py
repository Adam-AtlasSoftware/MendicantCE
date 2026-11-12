# usage: island_stats.py model.gltf bitmaps_root -> per shader x body part: tris, share of UV area, multipurpose R/G/B means, B>0.5 coverage, mean diffuse RGB
# (class_masks.py's gate reads these statistics: prism_packs.py's class_masks step measures them on the player's own
# extract, art/island_stats.py <model.gltf> <extract>/<map>, into <work>/class_masks/island_stats/<model>.txt)
import sys,os,runpy,collections,tempfile
import numpy as np
from PIL import Image,ImageDraw
gp=sys.argv[1]
os.environ['BITMAPS']=sys.argv[2]
_scratch=tempfile.mkdtemp(dir=os.environ.get('PRISM_PACKS_WORK') or None)
sys.argv=[sys.argv[0],gp,os.path.join(_scratch,'islands')]
import contextlib,io
with contextlib.redirect_stdout(io.StringIO()):
    G=runpy.run_path(os.path.join(os.path.dirname(os.path.abspath(__file__)),'uv_islands.py'))
res,g=G['res'],G['g']; BASE=os.environ['BITMAPS']
import shutil; shutil.rmtree(_scratch,ignore_errors=True)
def grp(b):
    b=b.replace('bip01 ','').replace('l ','').replace('r ','') if b[7:9] in ('l ','r ') or b.startswith('bip01 l ') or b.startswith('bip01 r ') else b.replace('bip01 ','')
    return b
agg=collections.defaultdict(lambda:collections.defaultdict(lambda:dict(tris=0,n=0,px=0,r=0.,g=0.,b=0.,bcov=0.,rgb=np.zeros(3),dpx=0)))
for (sh,rp),L in res.items():
    e=[m for m in g['materials'] if m.get('extras',{}).get('shader')==sh][0]['extras']['maps']
    if 'base_map' not in e: continue
    bpng=os.path.join(BASE,e['base_map']['png']); mpng=os.path.join(BASE,e['multipurpose_map']['png']) if 'multipurpose_map' in e else None
    if not os.path.exists(bpng): continue
    dif=Image.open(bpng).convert('RGB'); D=np.array(dif).astype(float)
    M=np.array(Image.open(mpng).convert('RGB')).astype(float) if mpng and os.path.exists(mpng) else None
    for d in L:
        top=grp(d['bones'][0][0])
        W,H=dif.size
        mask=Image.new('L',(W,H),0); dr=ImageDraw.Draw(mask)
        for t in d['idx']:
            dr.polygon([(d['uv'][i][0]*W,d['uv'][i][1]*H) for i in t],fill=1)
        mk=np.array(mask).astype(bool)
        n=mk.sum()
        if n==0: continue
        a=agg[sh][top]; a['tris']+=d['tris']; a['n']+=1; a['dpx']+=n; a['rgb']+=D[mk].sum(0)
        if M is not None:
            mw=Image.fromarray(mk.astype(np.uint8)*255).resize((M.shape[1],M.shape[0]),Image.NEAREST); mm=np.array(mw)>0
            k=mm.sum(); a['px']+=k; a['r']+=M[...,0][mm].sum(); a['g']+=M[...,1][mm].sum(); a['b']+=M[...,2][mm].sum(); a['bcov']+=(M[...,2][mm]>127).sum()
for sh,dd in agg.items():
    print('##',sh)
    for top,a in sorted(dd.items(),key=lambda x:-x[1]['dpx']):
        px=max(a['px'],1); rgb=a['rgb']/a['dpx']
        print('  %-10s isl=%2d tris=%4d  uvpx=%6d  R=%3.0f G=%3.0f B=%3.0f  B>.5=%3.0f%%  diffuse=(%3.0f,%3.0f,%3.0f)'%(top,a['n'],a['tris'],a['dpx'],a['r']/px,a['g']/px,a['b']/px,100*a['bcov']/px,*rgb))
