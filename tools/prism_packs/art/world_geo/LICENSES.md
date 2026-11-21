# The rock scans: sources and licences

The world geometry builder's rock relief (`wg2_natural.py`'s `scan_relief`) and the boulders' grain (`../prism_cast/rock_remodel.py`) read the height maps of the CC0 scans below. Nothing of them is in this repository: `wg2_scans.py fetch` downloads each from its source and refuses it unless its SHA-256 is the one here (also in `wg2_scans.json`); `wg2_scans.py prepare` turns it into the height field the builder reads.

Every scan is CC0 1.0 Universal (public domain dedication, <https://creativecommons.org/publicdomain/zero/1.0/>): free to use, change and ship; the credits are a courtesy. None comes from Quixel, Megascans, Fab or any source that is not freely redistributable.

| Scan | Asset | Source | Authors | Page | Licence | Tile | File | SHA-256 |
|---|---|---|---|---|---|---|---|---|
| `acg_Rock022` | Rock022 | ambientCG | Lennart Demes | <https://ambientcg.com/view?id=Rock022> | CC0 1.0 | 2.0 m | <https://ambientcg.com/get?file=Rock022_2K-JPG.zip> (member *_Displacement.jpg) | `b14568e5ccb4f6aae646df2e3afcc35768e818a78c246e93e0374230147bf2a5` |
| `acg_Rock042L` | Rock042L | ambientCG | Lennart Demes | <https://ambientcg.com/view?id=Rock042L> | CC0 1.0 | 2.0 m | <https://ambientcg.com/get?file=Rock042L_2K-JPG.zip> (member *_Displacement.jpg) | `5212ced8445c1402d37e9d517e02dc59edd117fed62d9b700286725b308451ca` |
| `ph_cliff_side` | Cliff Side | Poly Haven | James Ray Cock, Dario Barresi (photography); Jenelle van Heerden (processing) | <https://polyhaven.com/a/cliff_side> | CC0 1.0 | 1.83 m | <https://dl.polyhaven.org/file/ph-assets/Textures/png/2k/cliff_side/cliff_side_disp_2k.png> | `be974eb7ef5ecb17be1a574c8cadf09d47794983cacc0c27e953bbe47dedfd6f` |
| `ph_marble_cliff_03` | Marble Cliff 03 | Poly Haven | Amal Kumar | <https://polyhaven.com/a/marble_cliff_03> | CC0 1.0 | 5.748 m | <https://dl.polyhaven.org/file/ph-assets/Textures/png/2k/marble_cliff_03/marble_cliff_03_disp_2k.png> | `78a86415156d2ee9c22ad7dadce322dcbfd8bf18e2973e4b237d1aae80befdc5` |

ambientCG publishes no tile size; the 2 m given for its scans is ours, from their grain. Checked 2026-11-21.
