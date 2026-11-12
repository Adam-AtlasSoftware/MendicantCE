"""wg2_cracks.py <bsp.gltf> <file.pwg> [near|far|stock]: what the remodel draws (each surface's near or far triangles,
the stock materials the file leaves out drawn stock) welded at 1e-4 wu, and every open edge that is not on the stock
BSP's own border (nor a transparent surface's edge) classified: a T-junction (its partner across is another open edge
that runs on past a vertex: the rasteriser leaves pixels along it uncovered, a line of sparkles where the sky or the
level behind shows), or a gap (nothing within 2 mm: a hole). Per class of material, with the pairs of materials the
T-junctions join; the edges are saved to <file>.<mode>.seams.npz. "stock" draws the map's own BSP alone (the floor:
a few open edges). wg2_seal.py's output should show no T-junction in either mode.

Before wg2_seal.py, b30a near: 24,319 T-junction edges (8,406 wu); b30b: 76,857; a10a: 88,860; a30_a: 46,173. After:
0, 24, 181 and 0 (b30a far: 0, against the chord far level's 28,136 and 161,692 gap edges)."""
import sys
from collections import defaultdict

import numpy as np

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import wg_common as wg  # noqa: E402
from wg2_seal import read_pwg as _read  # noqa: E402


def read_pwg(path):
    _, _, parts = _read(path)
    for p in parts:
        p["P"] = p["verts"][:, 0:3]
    return {"parts": parts}

TOL = 2e-3


class SegGrid:
    def __init__(self, A, B, g=0.5):
        self.A, self.B, self.g = A, B, g
        cells = defaultdict(list)
        L = np.linalg.norm(B - A, axis=1)
        for i in range(len(A)):
            n = int(np.ceil(L[i] / (g * 0.5))) + 1
            t = np.linspace(0, 1, n)[:, None]
            pts = A[i] + t * (B[i] - A[i])
            for c in set(map(tuple, np.floor(pts / g).astype(np.int64))):
                cells[c].append(i)
        self.cells = {k: np.array(v) for k, v in cells.items()}

    def query(self, p, exclude=-1):
        """distance from p to the nearest segment (not `exclude`) and its index; inf when none within a cell"""
        c = np.floor(p / self.g).astype(np.int64)
        cand = []
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    v = self.cells.get((c[0] + dx, c[1] + dy, c[2] + dz))
                    if v is not None:
                        cand.append(v)
        if not cand:
            return np.inf, -1
        cand = np.unique(np.concatenate(cand))
        cand = cand[cand != exclude]
        if not len(cand):
            return np.inf, -1
        a, b = self.A[cand], self.B[cand]
        ab = b - a
        t = np.clip(((p - a) * ab).sum(1) / np.maximum((ab * ab).sum(1), 1e-18), 0, 1)
        d = np.linalg.norm(a + t[:, None] * ab - p, axis=1)
        k = int(np.argmin(d))
        return float(d[k]), int(cand[k])


def open_edges(P, T):
    key = np.round(P / 1e-4).astype(np.int64)
    _, first, wid = np.unique(key, axis=0, return_index=True, return_inverse=True)
    wid = wid.ravel()
    W = P[first]
    F = wid[T]
    tri_of = np.repeat(np.arange(len(T)), 3)
    E = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
    eo = np.concatenate([np.arange(len(T))] * 3)
    E = np.sort(E, 1)
    good = E[:, 0] != E[:, 1]
    E, eo = E[good], eo[good]
    u, inv, c = np.unique(E, axis=0, return_inverse=True, return_counts=True)
    once = c[inv.ravel()] == 1
    return W, E[once], eo[once]


def main():
    gltf, pwg = sys.argv[1], sys.argv[2]
    mode = sys.argv[3] if len(sys.argv) > 3 else "near"
    info, mats = wg.load_bsp(gltf)
    f = read_pwg(pwg)
    key_of = {(m.lightmap_index, m.material_index): m for m in mats}
    P, T, tag = [], [], []  # tag: material key and 'r'/'s'
    base = 0
    done = set()
    tags = []
    for p in (f["parts"] if mode != "stock" else []):
        k = (p["lm"], p["mi"])
        done.add(k)
        sel = []
        for s in range(p["psc"]):
            if mode == "far":
                a, n = p["pairs"][s]
            else:
                a, n = p["sft"][s], p["sft"][s + 1] - p["sft"][s]
            sel.append(p["tri"][a:a + n])
        tri = np.concatenate(sel) if sel else np.zeros((0, 3), np.int64)
        P.append(p["P"].astype(np.float64))
        T.append(tri + base)
        tags += [("r", k)] * len(tri)
        base += len(p["P"])
    border_tris = []
    SP, ST, sb = [], [], 0
    for m in mats:
        k = (m.lightmap_index, m.material_index)
        if m.shader_group != "senv":
            border_tris.append(m.position.astype(np.float64)[m.triangles])
            continue
        SP.append(m.position.astype(np.float64))
        ST.append(m.triangles + sb)
        sb += m.vertex_count
        if k not in done:
            P.append(m.position.astype(np.float64))
            T.append(m.triangles + base)
            tags += [("s", k)] * len(m.triangles)
            base += m.vertex_count
    P = np.concatenate(P)
    T = np.concatenate(T)
    W, oe, eo = open_edges(P, T)
    SW, soe, _ = open_edges(np.concatenate(SP), np.concatenate(ST))
    # segments that are a fine place for an open edge: the stock's own open edges and the transparent surfaces' edges
    bt = np.concatenate(border_tris) if border_tris else np.zeros((0, 3, 3))
    okA = np.concatenate([SW[soe[:, 0]], bt[:, 0], bt[:, 1], bt[:, 2]])
    okB = np.concatenate([SW[soe[:, 1]], bt[:, 1], bt[:, 2], bt[:, 0]])
    ok = SegGrid(okA, okB)
    A, B = W[oe[:, 0]], W[oe[:, 1]]
    L = np.linalg.norm(B - A, axis=1)
    on_border = np.zeros(len(oe), bool)
    for i in range(len(oe)):
        m = 0.5 * (A[i] + B[i])
        if ok.query(m)[0] < TOL and ok.query(A[i] * 0.8 + B[i] * 0.2)[0] < TOL:
            on_border[i] = True
    rest = np.nonzero(~on_border)[0]
    rg = SegGrid(A[rest], B[rest])
    kind = np.full(len(oe), "border", dtype=object)
    gapd = np.zeros(len(oe))
    pair_count = defaultdict(int)
    for j, i in enumerate(rest):
        m = 0.5 * (A[i] + B[i])
        d, pj = rg.query(m, exclude=j)
        gapd[i] = d
        if d < TOL:
            pt = tags[eo[rest[pj]]]
            mt = key_of.get(pt[1])
            pair_count[(tags[eo[i]][0], pt[0], key_of[tags[eo[i]][1]].shader.split("\\")[-1] if tags[eo[i]][1] in key_of else "?", (mt.shader.split("\\")[-1] if mt else "?"))] += 1
        kind[i] = "tjunction" if d < TOL else "gap"
    who = defaultdict(lambda: defaultdict(lambda: [0, 0.0]))
    mat_by_key = key_of
    for i in range(len(oe)):
        t = tags[eo[i]]
        m = mat_by_key.get(t[1])
        name = (t[0], m.shader.split("\\")[-1] if m else "?")
        who[kind[i]][name][0] += 1
        who[kind[i]][name][1] += L[i]
    print(f"{pwg.split('/')[-1]} mode={mode}: triangles {len(T)}, open edges {len(oe)}: border {int(on_border.sum())}, "
          f"T-junction {int((kind == 'tjunction').sum())} ({L[kind == 'tjunction'].sum():.1f} wu), "
          f"gap {int((kind == 'gap').sum())} ({L[kind == 'gap'].sum():.1f} wu)")
    g = gapd[kind == "gap"]
    if len(g):
        fin = g[np.isfinite(g)]
        print("  gap distance: inf %d, p50 %.4f p90 %.4f max %.4f" % ((~np.isfinite(g)).sum(),
              *(np.percentile(fin, [50, 90]).tolist() if len(fin) else [0, 0]), fin.max() if len(fin) else 0))
    for k in ("tjunction", "gap"):
        rows = sorted(who[k].items(), key=lambda x: -x[1][1])[:10]
        for name, (n, l) in rows:
            print(f"  {k:9s} {name[0]} {name[1]:40s} {n:7d} edges {l:9.1f} wu")
    for kk, v in sorted(pair_count.items(), key=lambda x: -x[1])[:12]:
        print("  pair", kk, v)
    np.savez_compressed(pwg + f".{mode}.seams.npz", A=A, B=B, kind=kind.astype(str), tag=np.array(
        [tags[e][0] for e in eo]))


if __name__ == "__main__":
    main()
