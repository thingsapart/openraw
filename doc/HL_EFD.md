### The Execution Flow Diagram (EFD) Format

The Execution Flow Diagram (EFD) is a schedule visualization format that merges the structural clarity of a loop tree with the detail of a schedule card, using terse syntax for easy "diffing" and analysis.

#### Formal Syntax

```
<Indentation> │
<Indentation> ├─ FUNC: <name> @ <compute_var> | S: <store_var> | D: <directives>
<Indentation> │         └── Rationale: <terse explanation>
<Indentation> │
<Indentation> ├─ LOOP: var=<var_name> [dim] >>(directive)
<Indentation> │         └── Rationale: <terse explanation>
```

#### Node Types

**1. FUNC Node:**
*   **`FUNC: <name>`**: The `Func` being computed.
*   **`@ <compute_var>`**: The variable at which the `Func` is computed (`compute_at`). `@ root` for `compute_root()`.
*   **`S: <store_var>`**: The storage location. `S: -><var>` for `store_at()`. `S: (fused)` means the `Func` is inlined (computed in registers).
*   **`D: <directives>`**: Other directives not captured in `compute_at` or `store_at` (e.g., `vectorize(x)`, `unroll(c)`, `gpu_threads(x, y)`).

**2. LOOP Node:**
*   **`LOOP: var=<var_name>`**: The loop variable name (e.g., `yo`, `xi`).
*   **`[dim]`**: Conceptual description of the loop (e.g., `[strip:32]`, `[vector]`).
*   **`>>(directive)`**: The primary directive applied to the loop (`>>parallel`, `>>vectorize`, `>>unroll`).

#### Common Abbreviations

*   `S: (fused)` = Inlined / Fused
*   `S: (output)` = Final output buffer
*   `@ <var>` = `compute_at(<var>)`
*   `D: unroll(c)` = `unroll(c)`
*   `D: vectorize(x)` = `vectorize(x)`
*   `D: reorder(...)` = `reorder(...)`

---

### Tips for Creating and Interpreting the EFD

#### 1. What to Include

*   **Only include scheduled or defined Funcs:** Do not include `Func`s that are simply expressions and are immediately fused by default.
*   **Include all `compute_at` and `store_at` relationships:** These are the core decisions that define the loop nest.
*   **Include all loops that received a directive:** Parallel, vectorized, or unrolled loops.
*   **Keep the Rationale Terse:** Explain *why* the decision was made in a single sentence if possible (e.g., "Max locality for fusion," "Parallelizes across cores").

#### 2. Interpreting Loop Nodes

*   **Understanding `>>(directive)`:** This tells you the nature of the loop.
    *   `>>parallel`: This loop will be executed concurrently on multiple cores.
    *   `>>vectorize` or `>>unroll`: The loop is very fast because the compiler will generate wider instructions or unroll the loop body to avoid loop overhead.

*   **Var Naming:**
    *   `var=y.yo` means the loop is the outer variable from a split on `y`.
    *   `var=y.yi` means the loop is the inner variable from a split on `y`.

#### 3. Interpreting FUNC Nodes

*   **Focus on the `@ <compute_var>`:** This is the `compute_at` location and determines where the computation begins relative to its consumer's loop nest.
*   **Crucial: The Storage Location (`S:`)**
    *   **`S: (fused)`**: **High Performance.** This is the preferred state. The computation is inlined and occurs in registers. Data does not hit main memory.
    *   **`S: -><var>`**: **Memory Store/Load Cost.** The `Func` will be fully computed and stored to a temporary memory buffer (at the `store_at` location), then loaded later by its consumer. This breaks fusion and is often a major performance bottleneck, as seen in Schedule B.

#### 4. The "Diff" Test

*   The true power of this format is in `git diff`. A change in indentation means a change in `compute_at` location (a major change in locality). A change in the `S:` field (e.g., from `(fused)` to `->yo`) signals a major performance change (fusion loss).

*   **Example from Schedule A vs B:**
    ```diff
    --- FUNC: demosaiced @ x | S: (fused) | D: unroll(c)
    +++ FUNC: demosaiced @ yi | S: ->yo
    ```
    This diff clearly and succinctly shows that the `demosaiced` stage was moved from a fused, register-based computation to a materialized, memory-based computation at the `yi` loop level, explaining the performance regression.
