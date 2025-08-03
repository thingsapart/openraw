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

NOTES: The **depth** of this CEFD immediately signals a highly fused, efficient pipeline. The frequent `S: (fused)` attribute confirms that many stages are computed in-register, avoiding costly memory access.

```
Compact Execution Flow Diagram (CEFD) - Schedule A (High-Performance)
└─ FUNC processed @ root | S: (output)                            # Final output, consumes parallel strips
  └─ LOOP var=yo [strip:32] >>parallel                            # Top-level parallelization across CPU cores
    ├─ FUNC denoised @ yo | S: ->yo                               # Computed once per strip at the coarsest level
    ├─ FUNC ca_corrected @ yo | S: ->yo                           # Consumes `denoised`, runs once per strip
    ├─ FUNC deinterleaved @ yo | S: ->yo | D: vectorize(x)        # Consumes `ca_corrected`, prepares data for demosaic
    └─ LOOP var=yi [tile]                                         # Inner loop over scanlines for producer-consumer locality
      └─ FUNC curved @ yi | S: ->yo | D: vectorize(x)             # Primary consumer, drives fusion of stages below
        ├─ FUNC sharpened @ x | S: (fused)                        # Fused into consumer (`processed` via `curved`)
        ├─ FUNC corrected @ x | S: (fused) | D: unroll(c)         # Fused into `curved`. `unroll(c)` for register-based RGB
        └─ FUNC demosaiced @ x | S: (fused) | D: unroll(c)        # Fused into `corrected`. All intermediates also fused
```

---

### Schedule B: The Intermediate (Slower) Refactored Schedule

NOTES: The **flatness** of this CEFD is an immediate red flag. All `FUNC` nodes are peers, and the repeated `@ yi | S: ->yo` pattern clearly shows that fusion is broken, forcing each stage to be written to and read from memory—a classic "waterfall" schedule that explains the severe performance regression.

```
Compact Execution Flow Diagram (CEFD) - Schedule B (Low-Performance)
└─ FUNC processed @ root | S: (output)                            # Final output, consumes parallel strips
  └─ LOOP var=yo [strip:32] >>parallel                            # Top-level parallelization across CPU cores
    └─ LOOP var=yi [tile]                                         # BOTTLENECK: All stages computed here, breaking fusion
      ├─ FUNC ca_corrected @ yi | S: ->yo                         # SLOW: Writes entire strip to a temporary buffer
      ├─ FUNC deinterleaved @ yi | S: ->yo                        # SLOW: Loads `ca_corrected`, writes its own temp buffer
      ├─ FUNC demosaiced @ yi | S: ->yo                           # SLOW: Loads `deinterleaved`, writes its own temp buffer
      ├─ FUNC corrected @ yi | S: ->yo                            # SLOW: Loads `demosaiced`, writes its own temp buffer
      ├─ FUNC curved @ yi | S: ->yo                               # SLOW: Loads `corrected`, writes its own temp buffer
      └─ FUNC sharpened @ yi | S: ->yo                           # SLOW: Loads `curved`, writes its own temp buffer
```
