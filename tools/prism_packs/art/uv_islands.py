# usage: uv_islands.py model.gltf out_prefix  -> per material (base map) UV-island table (bbox, tris, dominant bone) and overlay png
import sys,json,struct,os,collections
import numpy as np
from PIL import Image,ImageDraw
gp,outp=sys.argv[1],sys.argv[2]
g=json.load(open(gp)); base=os.path.dirname(gp)
buf={}
def rb(i):
    if i not in buf: buf[i]=open(os.path.join(base,g['buffers'][i]['uri']),'rb').read()
    return buf[i]
CT={5120:'b',5121:'B',5122:'h',5123:'H',5125:'I',5126:'f'}
NC={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}
def acc(i):
    a=g['accessors'][i]; bv=g['bufferViews'][a['bufferView']]
    n=NC[a['type']]; dt=np.dtype(CT[a['componentType']]).newbyteorder('<')
    st=bv.get('byteStride',n*dt.itemsize); o=bv.get('byteOffset',0)+a.get('byteOffset',0)
    raw=rb(bv['buffer'])
    arr=np.ndarray((a['count'],n),dtype=dt,buffer=raw,offset=o,strides=(st,dt.itemsize))
    arr=arr.astype(np.float64) if a.get('normalized') is None else arr.astype(np.float64)
    if a.get('normalized'):
        arr=arr/ {5121:255.,5123:65535.,5120:127.,5122:32767.}[a['componentType']]
    return arr
skin=g['skins'][0]; jn=[g['nodes'][j]['name'] for j in skin['joints']]
nodes=g['nodes']
def walk(i,path,out):
    n=nodes[i]; p=path+[n['name']]
    if 'mesh' in n and p[-1].endswith('superhigh'): out.append((n['name'],n['mesh']))
    for c in n.get('children',[]): walk(c,p,out)
out=[]
for r in g['scenes'][0]['nodes']: walk(r,[],out)
res=collections.defaultdict(list)
for path,mi in out:
    # path like model/lod_superhigh/region/perm/superhigh
    parts=path.split('/'); regperm='/'.join(parts[-3:-1])
    for pr in g['meshes'][mi]['primitives']:
        mat=g['materials'][pr['material']]; e=mat.get('extras',{})
        if not e.get('shader'): continue
        uv=acc(pr['attributes']['TEXCOORD_0']); idx=acc(pr['indices'])[:,0].astype(int).reshape(-1,3)
        J=acc(pr['attributes']['JOINTS_0']).astype(int); W=acc(pr['attributes']['WEIGHTS_0'])
        # union-find on vertices
        par=list(range(len(uv)))
        def f(x):
            while par[x]!=x: par[x]=par[par[x]]; x=par[x]
            return x
        for t in idx:
            a,b,c=f(t[0]),f(t[1]),f(t[2]); par[b]=a; par[f(c)]=a
        comp=collections.defaultdict(list)
        for ti,t in enumerate(idx): comp[f(t[0])].append(ti)
        for k,tris in comp.items():
            vs=np.unique(idx[tris].ravel())
            u=uv[vs]; su=np.floor(u[:,0].min()+1e-3); sv=np.floor(u[:,1].min()+1e-3); u=u-[su,sv]; bw=collections.Counter()
            for v in vs:
                for j,w in zip(J[v],W[v]):
                    if w>0: bw[jn[j]]+=w
            tot=sum(bw.values()) or 1
            top=[(n,round(w/tot,2)) for n,w in bw.most_common(3)]
            res[(e['shader'],regperm)].append(dict(tris=len(tris),u=(float(u[:,0].min()),float(u[:,0].max())),v=(float(u[:,1].min()),float(u[:,1].max())),bones=top,idx=idx[tris],uv=uv-[su,sv]))
json.dump({'%s | %s'%k:[{x:y for x,y in d.items() if x not in('idx','uv')} for d in v] for k,v in res.items()},open(outp+'_islands.json','w'),indent=1)
for (sh,rp),L in res.items():
    print('##',sh,'|',rp)
    for d in sorted(L,key=lambda d:-d['tris']):
        print('   tris %4d  u %.2f-%.2f v %.2f-%.2f  %s'%(d['tris'],*d['u'],*d['v'],d['bones']))

# ---- overlay per base map ----
BASEDIR=os.environ['BITMAPS']
palette=[(255,80,80),(80,255,80),(90,150,255),(255,255,0),(255,0,255),(0,255,255),(255,160,0),(180,120,255),(255,255,255)]
for (sh,rp),L in res.items():
    e=[m for m in g['materials'] if m.get('extras',{}).get('shader')==sh][0]['extras']
    bm=e['maps'].get('base_map') or e['maps'].get('transparent.maps[0].map')
    png=os.path.join(BASEDIR,bm['png'])
    if not os.path.exists(png): continue
    im=Image.open(png).convert('RGB'); S=1024; im=im.resize((S,S))
    im=Image.blend(im,Image.new('RGB',im.size,(0,0,0)),0.35)
    dr=ImageDraw.Draw(im); cols={}
    for d in L:
        top=d['bones'][0][0].replace('bip01 ','')
        c=cols.setdefault(top,palette[len(cols)%len(palette)])
        for t in d['idx']:
            pts=[(d['uv'][i][0]*S,d['uv'][i][1]*S) for i in t]
            dr.polygon(pts,outline=c)
        if d['tris']>=6:
            cu=(d['u'][0]+d['u'][1])/2*S; cv=(d['v'][0]+d['v'][1])/2*S
            dr.text((cu-20,cv-5),top,fill=(255,255,255))
    fn='%s_%s__%s.png'%(outp,sh.split('\\')[-1].replace(' ','_'),rp.replace('/','_'))
    im.save(fn); print('wrote',fn)
