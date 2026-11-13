#!/usr/bin/env python3
"""Resolve picks on hero_pick.py's views: the first surface hit under each pixel (world point, triangle,
shader, base-map coordinate in 1024 space with v down), and write the 50 px grid versions of the views.

  python -I art/hero_pick_query.py <pick dir> [--grid] [view x y ...]
"""
import json
import sys

import numpy as np


class Picker:
    def __init__(self, d):
        m = np.load(d + "/mesh.npz")
        self.co, self.tri, self.uv, self.mat = m["co"], m["tri"], m["uv"], m["mat"]
        self.names = [str(x) for x in m["names"]]
        self.cams = json.load(open(d + "/cams.json"))

    def ray(self, view, x, y):
        c = self.cams[view]
        R, s = c["res"], c["scale"]
        o = np.array(c["loc"]) + np.array(c["right"]) * ((x / R - 0.5) * s) + np.array(c["up"]) * ((0.5 - y / R) * s)
        return o, np.array(c["fwd"])

    def pick(self, view, x, y):
        o, dr = self.ray(view, x, y)
        co, tri = self.co, self.tri
        a, b, cc = co[tri[:, 0]], co[tri[:, 1]], co[tri[:, 2]]
        e1, e2 = b - a, cc - a
        p = np.cross(dr, e2)
        det = (e1 * p).sum(1)
        ok = np.abs(det) > 1e-12
        inv = np.where(ok, 1 / np.where(ok, det, 1), 0)
        tv = o - a
        u = (tv * p).sum(1) * inv
        q = np.cross(tv, e1)
        w = (dr * q).sum(1) * inv
        t = (e2 * q).sum(1) * inv
        hit = ok & (u >= 0) & (w >= 0) & (u + w <= 1) & (t > 0)
        if not hit.any():
            return None
        i = np.nonzero(hit)[0][np.argmin(t[hit])]
        bc = np.array([1 - u[i] - w[i], u[i], w[i]])
        P = o + dr * t[i]
        U = (bc[:, None] * self.uv[i]).sum(0)
        n = np.cross(e1[i], e2[i])
        return {"tri": int(i), "p": P.tolist(), "n": (n / np.linalg.norm(n)).tolist(), "shader": self.names[self.mat[i]],
                "uv1024": [float(U[0] * 1024), float((1 - U[1]) * 1024)]}


def grid(d):
    import glob
    import os
    import cv2
    for f in glob.glob(os.path.join(d, "*.png")):
        b = os.path.basename(f)
        if b.startswith("g_"):
            continue
        im = cv2.imread(f)
        for g in range(0, im.shape[0], 50):
            col = (255, 0, 255) if g % 100 == 0 else (100, 0, 100)
            cv2.line(im, (g, 0), (g, im.shape[0]), col, 1)
            cv2.line(im, (0, g), (im.shape[1], g), col, 1)
            if g % 100 == 0:
                cv2.putText(im, str(g), (g + 2, 12), 0, 0.4, (0, 255, 255), 1)
                cv2.putText(im, str(g), (2, g + 12), 0, 0.4, (0, 255, 255), 1)
        cv2.imwrite(os.path.join(d, "g_" + b), im)


if __name__ == "__main__":
    d = sys.argv[1]
    a = sys.argv[2:]
    if a and a[0] == "--grid":
        grid(d)
        a = a[1:]
    pk = Picker(d)
    for k in range(0, len(a), 3):
        r = pk.pick(a[k], float(a[k + 1]), float(a[k + 2]))
        print(a[k], a[k + 1], a[k + 2], json.dumps(r))
