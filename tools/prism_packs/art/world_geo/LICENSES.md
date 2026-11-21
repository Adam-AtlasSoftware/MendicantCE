# The rock scans: sources and licences

The world geometry builder's rock relief (`wg2_ledges.py`'s ledges on b30a; `wg2_natural.py`'s `scan_relief`) and the boulders' grain (`../prism_cast/rock_remodel.py`) read the height maps of the CC0 scans below. Nothing of them is in this repository: `wg2_scans.py fetch` downloads each from its source and refuses it unless its SHA-256 is the one here (also in `wg2_scans.json`); `wg2_scans.py prepare` turns it into the height field the builder reads.

Every scan is CC0 1.0 Universal (public domain dedication, <https://creativecommons.org/publicdomain/zero/1.0/>): free to use, change and ship; the credits are a courtesy. None comes from Quixel, Megascans, Fab or any source that is not freely redistributable.

| Scan | Asset | Source | Authors | Page | Licence | Tile | File | SHA-256 |
|---|---|---|---|---|---|---|---|---|
| `acg_Rock022` | Rock022 | ambientCG | Lennart Demes | <https://ambientcg.com/view?id=Rock022> | CC0 1.0 | 2.0 m | <https://ambientcg.com/get?file=Rock022_2K-JPG.zip> (member *_Displacement.jpg) | `b14568e5ccb4f6aae646df2e3afcc35768e818a78c246e93e0374230147bf2a5` |
| `acg_Rock042L` | Rock042L | ambientCG | Lennart Demes | <https://ambientcg.com/view?id=Rock042L> | CC0 1.0 | 2.0 m | <https://ambientcg.com/get?file=Rock042L_2K-JPG.zip> (member *_Displacement.jpg) | `5212ced8445c1402d37e9d517e02dc59edd117fed62d9b700286725b308451ca` |
| `ph_cliff_side` | Cliff Side | Poly Haven | James Ray Cock, Dario Barresi (photography); Jenelle van Heerden (processing) | <https://polyhaven.com/a/cliff_side> | CC0 1.0 | 1.83 m | <https://dl.polyhaven.org/file/ph-assets/Textures/png/2k/cliff_side/cliff_side_disp_2k.png> | `be974eb7ef5ecb17be1a574c8cadf09d47794983cacc0c27e953bbe47dedfd6f` |
| `ph_marble_cliff_03` | Marble Cliff 03 | Poly Haven | Amal Kumar | <https://polyhaven.com/a/marble_cliff_03> | CC0 1.0 | 5.748 m | <https://dl.polyhaven.org/file/ph-assets/Textures/png/2k/marble_cliff_03/marble_cliff_03_disp_2k.png> | `78a86415156d2ee9c22ad7dadce322dcbfd8bf18e2973e4b237d1aae80befdc5` |

ambientCG publishes no tile size; the 2 m given for its scans is ours, from their grain. Checked 2026-11-21.

## Scanned boulder meshes (prototype, round 2 option C: `../prism_cast/rock_scan_boulder.py`)

Read only by the prototype tool (`rock_scan_boulder.py prepare`, as data: positions and triangles of the glTF's first
mesh; its textures are not used, the boulder keeps CE's painted base map). Not fetched by the pack tools yet; nothing
of them is in this repository. CC0 1.0, Poly Haven, checked 2026-11-21.

| Mesh | Asset | Authors | Page | File | SHA-256 |
|---|---|---|---|---|---|
| `ph_namaqualand_boulder_05` (boulder) | Namaqualand Boulder 05 | Jenelle van Heerden (modeling); Dario Barresi (photography) | <https://polyhaven.com/a/namaqualand_boulder_05> | <https://dl.polyhaven.org/file/ph-assets/Models/gltf/1k/namaqualand_boulder_05/namaqualand_boulder_05_1k.gltf> | `f8e5fd8fd981e889d621ba4e53170a52c47cd3053b8748ee7025ed1083f36a5f` |
| | | | | <https://dl.polyhaven.org/file/ph-assets/Models/gltf/8k/namaqualand_boulder_05/namaqualand_boulder_05.bin> | `a60ade71de8612c873f9112159ed5d93dcba58979007a0915bc10d551a9ed03c` |
| `ph_boulder_01` (boulder_doublewide) | Boulder 01 | Rico Cilliers | <https://polyhaven.com/a/boulder_01> | <https://dl.polyhaven.org/file/ph-assets/Models/gltf/1k/boulder_01/boulder_01_1k.gltf> | `51487934db898b4c24072780a451e34eca5c02e3ada24a62bbc600bbc4019e92` |
| | | | | <https://dl.polyhaven.org/file/ph-assets/Models/gltf/8k/boulder_01/boulder_01.bin> | `7f5b06503d62bd95bfe9c6b8a354a5cc8ff04a4d271a8cf87a3e3125db4232d7` |
| `ph_namaqualand_boulder_03` (boulder_doublewide, tried) | Namaqualand Boulder 03 | Jenelle van Heerden (modeling); Dario Barresi (photography) | <https://polyhaven.com/a/namaqualand_boulder_03> | <https://dl.polyhaven.org/file/ph-assets/Models/gltf/1k/namaqualand_boulder_03/namaqualand_boulder_03_1k.gltf> | `14560936aaad68da0c0fc549b54c67bba4cd573dfe5b07f92835b9ca044a0092` |
| | | | | <https://dl.polyhaven.org/file/ph-assets/Models/gltf/8k/namaqualand_boulder_03/namaqualand_boulder_03.bin> | `d24151021ed310fe3207f1b5964bff22b583e77d2617427a88b5dcbc0447a475` |

The cliffs' material trial (option B) uses Marble Cliff 03's 2k colour, normal, displacement, roughness and ambient
occlusion maps (SHA-256 in `work/openh1_eval/v4_rock_ref_runs/LICENSES.md`), through the terrain layers' tool, in a
trial pack outside the repository.
