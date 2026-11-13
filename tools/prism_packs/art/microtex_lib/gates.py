"""Tile gates, run on the shipped tile as decoded from the 8-bit pack."""
import math

import numpy as np

from . import core

LIMITS = {
    "seam_excess_max": 0.0,        # wrap edge rougher than the roughest interior column/row
    "mean_normal_deg_max": 1.0,
    "normal_len_err_max": 0.02,    # RGB normal decoded length
    "rg_outside_unit_max": 0,      # texels with x^2 + y^2 > 1 in the packed RG
    "cavity_mean_tol": 0.01,
    "rough_mean_tol": 0.02,
    "lowfreq_frac_max": 0.01,      # variance below k = 4 cycles/tile (wavelength > tile/4)
    "height_range_mm_max": 3.0,
    "fine_frac_max": 0.25,         # slope variance at wavelengths < 4 shipped texels
}


def seam_ratio(a):
    """Wrap-edge roughness relative to the roughest interior column/row.

    For each axis: mean |diff| across the wrap (last -> first) divided by the largest
    per-column (per-row) mean |diff| inside the tile. A seam makes the wrap pair an outlier
    (> 1); a seamless tile keeps it inside the interior distribution (<= 1). Comparing with
    the whole column distribution, not the global mean, keeps regular patterns (ribs, dot
    lattices) whose phase happens to sit on the border from reading as a seam.
    """
    ex = np.abs(a[:, 0] - a[:, -1]).mean()
    ey = np.abs(a[0, :] - a[-1, :]).mean()
    cx = np.abs(np.diff(a, axis=1)).mean(0)
    cy = np.abs(np.diff(a, axis=0)).mean(1)
    return max(ex / max(cx.max(), 1e-12), ey / max(cy.max(), 1e-12))


def run(recipe, ship, decoded, rgb_normal_len, colour_channels):
    N, cav, rough, rg2 = decoded
    h = ship["h"]
    tile = recipe["tile_mm"]
    n = h.shape[0]
    mn = N.reshape(-1, 3).mean(0)
    ang = math.degrees(math.acos(min(1.0, mn[2] / np.linalg.norm(mn))))
    seams = {k: seam_ratio(v) for k, v in
             (("height", h), ("nx", N[..., 0]), ("ny", N[..., 1]), ("cavity", cav), ("rough", rough))}
    lowf = {k: core.lowfreq_fraction(v) for k, v in
            (("height", h), ("nx", N[..., 0]), ("ny", N[..., 1]), ("cavity", cav), ("rough", rough))}
    fine = max(core.fine_fraction(N[..., 0], n / 4), core.fine_fraction(N[..., 1], n / 4))
    medwl = core.median_wavelength_mm(h, tile)
    band = recipe["feature_band_mm"]
    hr = float(h.max() - h.min())
    L = LIMITS
    res = {
        "seam_ratio": {k: round(float(v), 4) for k, v in seams.items()},
        "seam_excess": round(float(max(0.0, max(seams.values()) - 1)), 4),
        "mean_normal_deg_from_z": round(ang, 4),
        "normal_len_err_max": round(float(np.abs(rgb_normal_len - 1).max()), 4),
        "rg_outside_unit": int((rg2 > 1.0 + 1e-9).sum()),
        "cavity_mean": round(float(cav.mean()), 5),
        "rough_offset_mean": round(float(rough.mean()), 5),
        "rough_offset_std": round(float(rough.std()), 4),
        "cavity_std": round(float(cav.std()), 4),
        "lowfreq_frac": {k: float(f"{v:.2e}") for k, v in lowf.items()},
        "height_range_mm": round(hr, 4),
        "median_wavelength_mm": round(medwl, 3),
        "feature_band_mm": band,
        "fine_frac_below_4_texels": round(fine, 4),
        "colour_channels": colour_channels,
    }
    checks = {
        "no_seams": res["seam_excess"] <= L["seam_excess_max"],
        "unit_normal": res["normal_len_err_max"] <= L["normal_len_err_max"] and res["rg_outside_unit"] <= L["rg_outside_unit_max"],
        "flat_mean_normal": ang <= L["mean_normal_deg_max"],
        "cavity_centred": abs(cav.mean() - 1) <= L["cavity_mean_tol"],
        "rough_centred": abs(rough.mean()) <= L["rough_mean_tol"],
        "no_low_frequency": max(lowf.values()) <= L["lowfreq_frac_max"],
        "height_within_3mm": hr <= L["height_range_mm_max"],
        "features_in_band": band[0] <= medwl <= band[1],
        "features_resolvable": fine <= L["fine_frac_max"],
        "no_colour": colour_channels == 0,
    }
    res["checks"] = {k: bool(v) for k, v in checks.items()}
    res["pass"] = bool(all(checks.values()))
    return res
