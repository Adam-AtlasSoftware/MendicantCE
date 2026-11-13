"""Procedural microtexture library (PRISM). numpy only, all original procedural data.

Height (mm) is the source of truth. Every generator is periodic by construction (integer
frequencies on the tile, wrapped indices), so the tile tiles with no seam.

  core     FFT helpers: frequency grid, band-pass noise, periodic blur, high-pass, Worley
  gen      the generator primitives (orange peel, bead-blast, brushed, scratches, chips,
           hex grip dots, stipple, pebble/leather, rib-knit, weave, anodised grain, dust)
  maps     height -> normal / cavity / roughness offset, downsample, packing
  gates    tile gates (seams, flat mean normal, centred means, no low-frequency energy,
           feature sizes, colour-free)
  ktx2     minimal KTX2 writer/reader with a numpy BC5 encoder/decoder
"""
