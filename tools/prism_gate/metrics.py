"""The quality gates' measures (docs/prism_gates.md, "Metrics"), on frames as numpy arrays (height, width, 3) of 0-255
values (8-bit sRGB as the game draws them), each frame first brought to 960x540 as the path tracer's first gate
(pt_runs/scripts/gate2.py) measured them:

  error    relative MSE of a frame against its reference, where the scene did not move of itself
  blotch   mean |4-24 px band of (frame - reference)| over the reference's mean: patches of error the size of a
           filter's or a reservoir's footprint
  flicker  the mean change from step to step of a moving camera's frames, over the same of RT off's (no history)
  flip     mean FLIP (flip.py) against the reference
  trail    the frames a moving object's ghost lasts behind it, a still camera's (ghost_trail)

The regions left out of error and blotch are those where RT off's frame at the pose while moving and its still frame
there differ (water's waves, a projectile, a body): what moved of itself is not the light's.
"""
import numpy as np
from PIL import Image

SIZE = (960, 540)


def load(path):
    """an image file as float32 RGB at SIZE"""
    image = Image.open(path).convert("RGB")
    if image.size != SIZE:
        image = image.resize(SIZE, Image.BILINEAR)
    return np.asarray(image).astype(np.float32)


def gaussian_kernel(sigma):
    radius = int(3 * sigma)
    x = np.arange(-radius, radius + 1, dtype=np.float32)
    k = np.exp(-x * x / (2 * sigma * sigma))
    return k / k.sum()


def blur(g, sigma):
    """a 2D array blurred by a Gaussian of sigma pixels (its radius 3 sigma), the edges repeated"""
    k = gaussian_kernel(sigma)
    r = len(k) // 2
    p = np.pad(g, ((0, 0), (r, r)), mode="edge")
    rows = sum(w * p[:, i:i + g.shape[1]] for i, w in enumerate(k))
    p = np.pad(rows, ((r, r), (0, 0)), mode="edge")
    return sum(w * p[i:i + g.shape[0], :] for i, w in enumerate(k))


def band(a):
    """the 4-24 px band (blur 2 less blur 12) of an image's channel mean"""
    g = a.mean(2) if a.ndim == 3 else a
    return blur(g, 2) - blur(g, 12)


def keep_mask(off_moving, off_still):
    """where RT off's frame while moving and its still frame at the pose agree (widened by a blur of 4): what moves
    of itself left out; everything where less than a fifth would be kept"""
    still = np.abs(off_moving - off_still).mean(2)
    keep = blur(still, 4) < 3.0
    if keep.mean() < 0.2:
        keep[:] = True
    return keep


def rel_mse(a, ref, keep=None):
    keep = np.ones(a.shape[:2], bool) if keep is None else keep
    diff = a - ref
    return float((diff ** 2).mean(2)[keep].mean() / max((ref ** 2).mean(2)[keep].mean(), 1e-6))


def blotch(a, ref, keep=None):
    keep = np.ones(a.shape[:2], bool) if keep is None else keep
    return float(np.abs(band(a - ref))[keep].mean() / max(ref.mean(2)[keep].mean(), 1e-6))


def step_change(frames):
    """the mean absolute change from each frame to the next"""
    return float(np.mean([np.abs(b - a).mean() for a, b in zip(frames, frames[1:])]))


def flicker(moving, moving_off):
    return step_change(moving) / max(step_change(moving_off), 1e-6)


def luma(a):
    return a @ np.array([0.2126, 0.7152, 0.0722], np.float32) if a.ndim == 3 else a


def ghost_trail(frames, frames_off, lags=24, ratio=1.5, least=2.0):
    """A still camera's frames as an object crosses it: the frames its ghost lasts behind it.

    RT off's frames (no history) show where the object truly is: a pixel is the object's in frame s where RT off's
    frame s differs there from its frame t (by more than 8 levels). A pixel the object left k frames before t (last
    its in t-k) and does not cover again by t+lags+1 shows, in a frame with no ghost, what it shows once the object
    is long gone: the residual at lag k is the mean |frame t - frame t+lags+1| (luminance, 8-bit levels) over those
    pixels, over t. The trail is the last lag (1 to lags - 5) whose residual is past `ratio` times the far lags'
    (the last five's) and past `least`: 0 none. Returns (trail, residuals by lag)."""
    y = np.stack([luma(f) for f in frames])
    yo = np.stack([luma(f) for f in frames_off])
    later = lags + 1
    sums = np.zeros(lags)
    counts = np.zeros(lags)
    for t in range(lags + 1, len(y) - later):
        covered = np.abs(yo[t - lags - 1:t + later + 1] - yo[t]) > 8.0
        before = covered[:lags + 2]
        after = covered[lags + 2:]
        # (the frames since the object last covered the pixel (0: it does in t); lags + 2: none within the lags)
        last = np.where(before.any(0), np.argmax(before[::-1], 0), lags + 2)
        clear_after = ~after.any(0)
        diff = np.abs(y[t] - y[t + later])
        for k in range(1, lags + 1):
            where = (last == k) & clear_after
            sums[k - 1] += diff[where].sum()
            counts[k - 1] += where.sum()
    residuals = [float(s / c) if c else 0.0 for s, c in zip(sums, counts)]
    far = float(np.mean(residuals[-5:]))
    trail = 0
    for k, r in enumerate(residuals[:-5], start=1):
        if r > ratio * far and r > least:
            trail = k
    return trail, residuals


def sky_share(z):
    """the share of a reference's pixels with no surface (its Z channel 0: the sky, or a pose outside the level)"""
    return float((np.asarray(z) <= 0.0).mean())
