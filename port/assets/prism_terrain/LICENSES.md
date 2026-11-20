# The terrain pack's sources and licences

The terrain pack (`<data folder>/prism_terrain/layers.ptl`, built by `tools/prism_terrain_layers/prism_terrain_layers.py`
from `tools/prism_terrain_layers/manifest.json`) is made of the CC0 scans below and of our own work. None of it is
made of the game's data. The build writes this list again beside the pack as `LICENSES.txt`.

CC0 1.0 Universal (public domain dedication): <https://creativecommons.org/publicdomain/zero/1.0/>. Anything may be
done with these files, including shipping them with the game. The credits are a courtesy to their authors.

| Layer | Asset | Author | Page | Licence | Checked |
| --- | --- | --- | --- | --- | --- |
| sand | Ground054 | ambientCG (Lennart Demes) | <https://ambientcg.com/view?id=Ground054> | CC0 1.0 | 2026-11-19 |
| dirt | Ground103 | ambientCG (Lennart Demes) | <https://ambientcg.com/view?id=Ground103> | CC0 1.0 | 2026-11-19 |
| mud | Ground025 | ambientCG (Lennart Demes) | <https://ambientcg.com/view?id=Ground025> | CC0 1.0 | 2026-11-19 |
| rock | Rock023 | ambientCG (Lennart Demes) | <https://ambientcg.com/view?id=Rock023> | CC0 1.0 | 2026-11-19 |
| snow | Snow005 | ambientCG (Lennart Demes) | <https://ambientcg.com/view?id=Snow005> | CC0 1.0 | 2026-11-19 |
| ice | Snow014 | ambientCG (Lennart Demes) | <https://ambientcg.com/view?id=Snow014> | CC0 1.0 | 2026-11-19 |
| sand_desert (d40) | Ground080 | ambientCG (Lennart Demes) | <https://ambientcg.com/view?id=Ground080> | CC0 1.0 | 2026-11-19 |

Each archive is the asset's `2K-JPG` download, checked against the SHA-256 in the manifest before it is used.

Our own (this repository's licence): the macro-variation map (tileable value noise, made by the tool), the
processing (detail as the albedo over its own low-pass, height range, roughness over its mean), and every number in
the manifest (tile sizes, strengths).
