### The Compact Execution Flow Diagram (CEFD)

The CEFD is a textual visualization of a Halide schedule designed for high information density, easy comparison ("diffing"), and clear performance analysis.

#### Syntax

```
<indent><branch> <NODE_TYPE> <attributes...>  <padding>  # <note>
```

*   **Indentation:** Two spaces per level (`  `).
*   **Branch:** `└─` for the last child, `├─` for others.
*   **Node Types:** `LOOP` or `FUNC`.
*   **Attributes:** Pipe-separated (`|`). Prefixes (`@`, `S:`, `D:`) are used for clarity.
*   **Note:** Begins with `#`, aligned to a fixed column for readability.

---

### Schedule A: The Original High-Performance CPU Schedule

NOTES: The **depth** of this CEFD immediately signals a highly fused, efficient pipeline. The frequent `S: (fused)` attribute confirms that many stages are computed in-register, avoiding costly memory access. The `sharpened` stage, now operating in linear space, drives the fusion for the early parts of the pipeline.

```
Compact Execution Flow Diagram (CEFD) - Schedule A (High-Performance)
└─ FUNC processed @ root | S: (output)                            # Final output, consumes parallel strips
  └─ LOOP var=yo [strip:32] >>parallel                            # Top-level parallelization across CPU cores
    ├─ FUNC denoised @ yo | S: ->yo                               # Computed once per strip at the coarsest level
    ├─ FUNC ca_corrected @ yo | S: ->yo                           # Consumes `denoised`, runs once per strip
    ├─ FUNC deinterleaved @ yo | S: ->yo | D: vectorize(x)        # Consumes `ca_corrected`, prepares data for demosaic
    └─ LOOP var=yi [tile]                                         # Inner loop over scanlines for producer-consumer locality
      └─ FUNC curved @ yi | S: ->yo | D: vectorize(x)             # Tone curve, primary consumer, drives fusion below
        └─ FUNC sharpened @ x | S: (fused)                        # Fused into `curved`. Consumes corrected/demosaiced
          ├─ FUNC luma @ x | S: (fused) | D: vectorize(x)          # Sharpen helper, fused.
          ├─ FUNC blurred_luma @ x | S: (fused) | D: vectorize(x)  # Sharpen helper, fused.
          ├─ FUNC luma_x @ y | S: ->y | D: vectorize(x)            # Sharpen helper, computed per scanline.
          ├─ FUNC corrected @ x | S: (fused) | D: unroll(c)         # Fused into `sharpened`.
          └─ FUNC demosaiced @ x | S: (fused) | D: unroll(c)        # Fused into `corrected`. All intermediates also fused
```

---

### Schedule D: Tiled Schedule with Dehaze and Local Laplacian

NOTES: This schedule adds the new pointwise `dehazed` stage. Crucially, it slots perfectly into the existing `xo` (per-tile) compute level alongside the other color manipulation stages (`srgb_to_lch`, `local_adjustments`, etc.). This preserves the pipeline's high performance, as the new stage is computed in-cache just before its consumer. The performance impact is negligible, demonstrating the power of scheduling pointwise operations.

```
Compact Execution Flow Diagram (CEFD) - Schedule D (Dehaze + Local Laplacian)
└─ FUNC final_stage @ root | S: (output)                               # Final output buffer
  ├─ FUNC cc_matrix @ root | S: (root)                                 # Global: Interpolated color matrix
  ├─ FUNC remap_detail_lut @ root | S: (root)                           # Global: LUT for local laplacian detail
  └─ FUNC tone_curve_func @ root | S: (root)                           # Global: LUT for final tone mapping
  └─ LOOP var=yo [strip:32] >>parallel                                  # Parallelize over large horizontal strips
    ├─ FUNC gPyramid_3..7 @ yo | S: ->yo | D: vectorize(x)              # SCHEDULE FORK: Low-freq pyramids computed per strip
    ├─ FUNC inGPyramid_3..7 @ yo | S: ->yo | D: vectorize(x)            # -> Manages large stencils, prevents root-level computation
    ├─ FUNC outGPyramid_7 @ yo | S: ->yo | D: vectorize(x)              # -> Coarsest output level also computed per strip
    ├─ FUNC demosaiced @ yo | S: ->yo | D: vectorize(x)                 # Upstream stages computed per strip
    ├─ FUNC deinterleaved @ yo | S: ->yo | D: vectorize(x)
    ├─ FUNC ca_corrected @ yo | S: ->yo | D: vectorize(x)
    ├─ FUNC denoised_raw @ yo | S: ->yo | D: vectorize(x)
    └─ LOOP var=xo [tile:256]                                            # Sequentially process tiles within each strip
      ├─ FUNC gPyramid_0..2 @ xo | S: ->xo | D: vectorize(xi)            # SCHEDULE FORK: High-freq pyramids computed per tile
      ├─ FUNC inGPyramid_0..2 @ xo | S: ->xo | D: vectorize(xi)          # -> Maximizes locality for small stencils
      ├─ FUNC lPyramid_0..7 @ xo | S: ->xo | D: vectorize(xi)            # -> All laplacian levels computed per tile
      ├─ FUNC outLPyramid_0..7 @ xo | S: ->xo | D: vectorize(xi)
      ├─ FUNC outGPyramid_0..6 @ xo | S: ->xo | D: vectorize(xi)
      ├─ FUNC lab @ xo | S: ->xo | D: vectorize(xi)                      # Color conversions computed per tile
      ├─ FUNC corrected @ xo | S: ->xo | D: vectorize(xi)                # Color correction computed per tile
      ├─ FUNC dehazed @ xo | S: ->xo | D: vectorize(xi)                  # NEW: Dehaze is pointwise, fits perfectly here
      ├─ FUNC sharpened @ xo | S: ->xo | D: vectorize(xi)                # Passthrough, but scheduled per tile
      ├─ FUNC local_adjustments @ xo | S: ->xo | D: vectorize(xi)        # Main adjustment stage computed per tile
      └─ LOOP var=yi [scanline]                                          # Innermost loops over pixels within a tile
        └─ LOOP var=xi [vector] >>vectorize
          └─ ... (pointwise funcs fused here)
```

---

### Schedule B: Low-Performance (The "Waterfall")

```
Compact Execution Flow Diagram (CEFD) - Schedule B (Low-Performance)
└─ FUNC processed @ root | S: (output)                            # Final output, consumes parallel strips
  └─ LOOP var=yo [strip:32] >>parallel                            # Top-level parallelization across CPU cores
    └─ LOOP var=yi [tile]                                         # BOTTLENECK: All stages computed here, breaking fusion
      ├─ FUNC ca_corrected @ yi | S: ->yo                         # SLOW: Writes entire strip to a temporary buffer
      ├─ FUNC deinterleaved @ yi | S: ->yo                        # SLOW: Loads `ca_corrected`, writes its own temp buffer
      ├─ FUNC demosaiced @ yi | S: ->yo                           # SLOW: Loads `deinterleaved`, writes its own temp buffer
      ├─ FUNC corrected @ yi | S: ->yo                            # SLOW: Loads `demoseiced`, writes its own temp buffer
      ├─ FUNC sharpened @ yi | S: ->yo                           # SLOW: Loads `corrected`, writes its own temp buffer
      └─ FUNC curved @ yi | S: ->yo                               # SLOW: Loads `sharpened`, writes its own temp buffer
