"""Minimal, dependency-free readers/writers for Halo CE JMS (8200) and JMA (16392).

Pure Python (no numpy, no bpy) so it runs in Blender's Python and in any other alike.
We write our own instead of driving the Halo Blender toolset add-on (not yet cleared to
run here).

Conventions (verified against the extractor's (map_extract's) output):
- JMS/JMA positions and translations are world units x 100 (1 world unit = 10 ft).
- JMS/JMA quaternions are (i, j, k, w) and are the CONJUGATE of the standard
  (glTF / Blender / mathutils) rotation of the same node. `std_quat()` converts.
- JMS UVs equal Blender's loop UVs (Blender v = 1 - glTF v).
- JMS triangle winding is the reverse of Blender/glTF winding (map_extract keeps the
  Xbox draw order in JMS and flips for glTF).
"""
from dataclasses import dataclass, field
from typing import List, Tuple


@dataclass
class JmsNode:
    name: str
    first_child: int
    next_sibling: int
    rot: Tuple[float, float, float, float]  # JMS convention (conjugated), i j k w
    pos: Tuple[float, float, float]         # x100 units


@dataclass
class JmsVertex:
    node0: int
    pos: Tuple[float, float, float]
    normal: Tuple[float, float, float]
    node1: int
    weight: float  # weight of node1
    u: float
    v: float


@dataclass
class JmsTriangle:
    region: int
    shader: int
    v: Tuple[int, int, int]


@dataclass
class Jms:
    version: int = 8200
    checksum: int = 0
    nodes: List[JmsNode] = field(default_factory=list)
    materials: List[Tuple[str, str]] = field(default_factory=list)
    markers: List[tuple] = field(default_factory=list)  # (name, region, parent, rot, pos, radius)
    regions: List[str] = field(default_factory=list)
    vertices: List[JmsVertex] = field(default_factory=list)
    triangles: List[JmsTriangle] = field(default_factory=list)

    def parents(self):
        """Parent index per node from the first-child / next-sibling links."""
        par = [-1] * len(self.nodes)
        for i, n in enumerate(self.nodes):
            c = n.first_child
            while c != -1:
                par[c] = i
                c = self.nodes[c].next_sibling
        return par


def std_quat(q):
    """JMS/JMA (i,j,k,w) -> standard (w,x,y,z) for mathutils.Quaternion."""
    i, j, k, w = q
    return (w, -i, -j, -k)


def jms_quat(wxyz):
    """standard (w,x,y,z) -> JMS (i,j,k,w)."""
    w, x, y, z = wxyz
    return (-x, -y, -z, w)


class _Tok:
    def __init__(self, text):
        self.lines = [ln.strip() for ln in text.splitlines()]
        self.i = 0

    def line(self):
        while self.i < len(self.lines) and self.lines[self.i] == "":
            self.i += 1
        s = self.lines[self.i]
        self.i += 1
        return s

    def raw_line(self):
        # names may legitimately be empty strings; JMS writes one item per line
        s = self.lines[self.i]
        self.i += 1
        return s

    def int(self):
        return int(self.line())

    def floats(self):
        return tuple(float(x) for x in self.line().split())


def read_jms(path) -> Jms:
    t = _Tok(open(path, encoding="utf-8", errors="replace").read())
    j = Jms()
    j.version = t.int()
    if j.version != 8200:
        raise ValueError("only JMS 8200 supported, got %d" % j.version)
    j.checksum = t.int()
    for _ in range(t.int()):
        name = t.line()
        fc = t.int()
        ns = t.int()
        rot = t.floats()
        pos = t.floats()
        j.nodes.append(JmsNode(name, fc, ns, rot, pos))
    for _ in range(t.int()):
        j.materials.append((t.line(), t.line()))
    for _ in range(t.int()):
        name = t.line()
        region = t.int()
        parent = t.int()
        rot = t.floats()
        pos = t.floats()
        radius = float(t.line())
        j.markers.append((name, region, parent, rot, pos, radius))
    for _ in range(t.int()):
        j.regions.append(t.line())
    for _ in range(t.int()):
        n0 = t.int()
        pos = t.floats()
        nrm = t.floats()
        n1 = t.int()
        w = float(t.line())
        u = float(t.line())
        v = float(t.line())
        t.line()  # unused third texcoord
        j.vertices.append(JmsVertex(n0, pos, nrm, n1, w, u, v))
    for _ in range(t.int()):
        reg = t.int()
        sh = t.int()
        vv = tuple(int(x) for x in t.line().split())
        j.triangles.append(JmsTriangle(reg, sh, vv))
    return j


def _f(x):
    s = "%.10g" % x
    return "0" if s in ("-0", "0") else s


def write_jms(path, j: Jms):
    out = [str(j.version), str(j.checksum), str(len(j.nodes))]
    for n in j.nodes:
        out += [n.name, str(n.first_child), str(n.next_sibling),
                "\t".join(_f(x) for x in n.rot), "\t".join(_f(x) for x in n.pos)]
    out.append(str(len(j.materials)))
    for a, b in j.materials:
        out += [a, b]
    out.append(str(len(j.markers)))
    for name, region, parent, rot, pos, radius in j.markers:
        out += [name, str(region), str(parent), "\t".join(_f(x) for x in rot),
                "\t".join(_f(x) for x in pos), _f(radius)]
    out.append(str(len(j.regions)))
    out += j.regions
    out.append(str(len(j.vertices)))
    for v in j.vertices:
        out += [str(v.node0), "\t".join(_f(x) for x in v.pos), "\t".join(_f(x) for x in v.normal),
                str(v.node1), _f(v.weight), _f(v.u), _f(v.v), "0"]
    out.append(str(len(j.triangles)))
    for tr in j.triangles:
        out += [str(tr.region), str(tr.shader), "\t".join(str(x) for x in tr.v)]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")


@dataclass
class Jma:
    version: int
    frame_count: int
    fps: int
    actor: str
    checksum: int
    node_names: List[str]
    first_child: List[int]
    next_sibling: List[int]
    # frames[f][n] = (pos(x100), rot JMS-convention i j k w, scale)
    frames: List[List[tuple]]

    def parents(self):
        par = [-1] * len(self.node_names)
        for i in range(len(self.node_names)):
            c = self.first_child[i]
            while c != -1:
                par[c] = i
                c = self.next_sibling[c]
        return par


def read_jma(path) -> Jma:
    t = _Tok(open(path, encoding="utf-8", errors="replace").read())
    ver = t.int()
    if ver < 16390:
        raise ValueError("unsupported JMA version %d" % ver)
    fc = t.int()
    fps = t.int()
    n_actors = t.int()
    actor = t.line()
    for _ in range(n_actors - 1):
        t.line()
    nn = t.int()
    checksum = t.int()
    names, fch, nsib = [], [], []
    for _ in range(nn):
        names.append(t.line())
        fch.append(t.int())
        nsib.append(t.int())
    frames = []
    for _ in range(fc):
        fr = []
        for _ in range(nn):
            pos = t.floats()
            rot = t.floats()
            sc = float(t.line())
            fr.append((pos, rot, sc))
        frames.append(fr)
    return Jma(ver, fc, fps, actor, checksum, names, fch, nsib, frames)
