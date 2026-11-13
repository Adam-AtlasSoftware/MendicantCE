"""LDR FLIP, the difference evaluator of Andersson, Nilsson, Akenine-Möller, Oskarsson, Åström and Fairchild, "FLIP:
A Difference Evaluator for Alternating Images" (Proceedings of the ACM on Computer Graphics and Interactive
Techniques, 2020), written here from the paper's description, in numpy: each pixel's perceived difference, 0 none
to 1 the most, between a reference and a test image of 8-bit sRGB.

The colour pipeline filters both images in the opponent space YCxCz by the contrast sensitivity functions (sums of
Gaussians, by the pixels a degree of the viewer's sight holds), takes them to L*a*b* with the Hunt adjustment and
measures the HyAB distance, raised to 0.7 and redistributed so that 95% of the range goes to the differences up to
0.4 of the largest possible (green against blue). The feature pipeline finds edges and points (the first and second
derivatives of a Gaussian of 0.082 degrees) in the achromatic channel and raises the colour difference to one less
the larger feature difference.

    flip_map(reference, test)   -> (height, width) array
    mean_flip(reference, test)  -> float
"""
import numpy as np

# the viewer: 0.7 m from a 0.7 m wide display of 3840 pixels (the paper's default)
PIXELS_PER_DEGREE = 0.7 * 3840 / 0.7 * np.pi / 180
QC, PC, PT, QF, FEATURE_WIDTH = 0.7, 0.4, 0.95, 0.5, 0.082
# the contrast sensitivity functions, per YCxCz channel: (a1, b1, a2, b2)
CSF = ((1.0, 0.0047, 0.0, 1e-5), (1.0, 0.0053, 0.0, 1e-5), (34.1, 0.04, 13.5, 0.025))

_RGB_TO_XYZ = np.array([[0.4124564, 0.3575761, 0.1804375],
                        [0.2126729, 0.7151522, 0.0721750],
                        [0.0193339, 0.1191920, 0.9503041]])
_XYZ_TO_RGB = np.linalg.inv(_RGB_TO_XYZ)
_WHITE = _RGB_TO_XYZ @ np.ones(3)


def _srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def _xyz_to_ycxcz(xyz):
    x, y, z = (xyz[..., i] / _WHITE[i] for i in range(3))
    return np.stack([116 * y - 16, 500 * (x - y), 200 * (y - z)], -1)


def _ycxcz_to_xyz(ycc):
    y = (ycc[..., 0] + 16) / 116
    x = ycc[..., 1] / 500 + y
    z = y - ycc[..., 2] / 200
    return np.stack([x * _WHITE[0], y * _WHITE[1], z * _WHITE[2]], -1)


def _xyz_to_lab(xyz):
    t = xyz / _WHITE
    delta = 6 / 29
    f = np.where(t > delta ** 3, np.cbrt(t), t / (3 * delta ** 2) + 4 / 29)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def _convolve(image, kernel):
    """a 2D image convolved with a 2D kernel (by FFT), the edges repeated"""
    ry, rx = kernel.shape[0] // 2, kernel.shape[1] // 2
    p = np.pad(image, ((ry, ry), (rx, rx)), mode="edge")
    shape = (p.shape[0] + kernel.shape[0] - 1, p.shape[1] + kernel.shape[1] - 1)
    full = np.fft.irfft2(np.fft.rfft2(p, shape) * np.fft.rfft2(kernel, shape), shape)
    h, w = image.shape
    return full[2 * ry:2 * ry + h, 2 * rx:2 * rx + w]


def _csf_kernels(ppd):
    b_most = max(max(b1, b2) for _, b1, _, b2 in CSF)
    radius = int(np.ceil(3 * np.sqrt(b_most / (2 * np.pi ** 2)) * ppd))
    d = np.arange(-radius, radius + 1) / ppd
    r2 = d[None, :] ** 2 + d[:, None] ** 2
    kernels = []
    for a1, b1, a2, b2 in CSF:
        g = a1 * np.sqrt(np.pi / b1) * np.exp(-np.pi ** 2 * r2 / b1) + \
            a2 * np.sqrt(np.pi / b2) * np.exp(-np.pi ** 2 * r2 / b2)
        kernels.append(g / g.sum())
    return kernels


def _feature_kernels(ppd):
    sd = 0.5 * FEATURE_WIDTH * ppd
    radius = int(np.ceil(3 * sd))
    x = np.arange(-radius, radius + 1, dtype=np.float64)
    xx, yy = np.meshgrid(x, x)
    g = np.exp(-(xx ** 2 + yy ** 2) / (2 * sd ** 2))
    out = []
    for kernel in (-xx * g, (xx ** 2 / sd ** 2 - 1) * g):
        negative, positive = -kernel[kernel < 0].sum(), kernel[kernel > 0].sum()
        out.append(np.where(kernel < 0, kernel / negative, kernel / positive))
    return out


def _hunt(lab):
    return np.stack([lab[..., 0], 0.01 * lab[..., 0] * lab[..., 1], 0.01 * lab[..., 0] * lab[..., 2]], -1)


def _hyab(a, b):
    return np.abs(a[..., 0] - b[..., 0]) + np.sqrt((a[..., 1] - b[..., 1]) ** 2 + (a[..., 2] - b[..., 2]) ** 2)


def flip_map(reference, test, ppd=PIXELS_PER_DEGREE):
    """the FLIP error of each pixel; reference and test: (height, width, 3) arrays of 0-255 sRGB"""
    ref = np.asarray(reference, np.float64) / 255.0
    tst = np.asarray(test, np.float64) / 255.0
    if ref.shape != tst.shape:
        raise ValueError(f"images of different sizes: {ref.shape} and {tst.shape}")
    ycc = [_xyz_to_ycxcz(_srgb_to_linear(im) @ _RGB_TO_XYZ.T) for im in (ref, tst)]
    # the colour pipeline
    csf = _csf_kernels(ppd)
    labs = []
    for image in ycc:
        filtered = np.stack([_convolve(image[..., c], csf[c]) for c in range(3)], -1)
        linear = np.clip(_ycxcz_to_xyz(filtered) @ _XYZ_TO_RGB.T, 0.0, 1.0)
        labs.append(_hunt(_xyz_to_lab(linear @ _RGB_TO_XYZ.T)))
    green = _hunt(_xyz_to_lab(_RGB_TO_XYZ @ np.array([0.0, 1.0, 0.0])))
    blue = _hunt(_xyz_to_lab(_RGB_TO_XYZ @ np.array([0.0, 0.0, 1.0])))
    cmax = _hyab(green, blue) ** QC
    distance = _hyab(labs[0], labs[1]) ** QC
    pccmax = PC * cmax
    colour = np.where(distance < pccmax, PT / pccmax * distance,
                      PT + (distance - pccmax) / (cmax - pccmax) * (1 - PT))
    # the feature pipeline
    edge, point = _feature_kernels(ppd)
    features = []
    for image in ycc:
        y = (image[..., 0] + 16) / 116
        features.append([np.hypot(_convolve(y, k), _convolve(y, k.T)) for k in (edge, point)])
    feature = np.maximum(np.abs(features[0][0] - features[1][0]), np.abs(features[0][1] - features[1][1]))
    feature = (feature / np.sqrt(2)) ** QF
    return colour ** (1 - feature)


def mean_flip(reference, test, ppd=PIXELS_PER_DEGREE):
    return float(flip_map(reference, test, ppd).mean())
