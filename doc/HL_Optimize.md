### The Core Philosophy: A Good Schedule Minimizes and Optimizes Data Movement

Remember, arithmetic is cheap; memory access is expensive. A good schedule is an assembly line that keeps data flowing through the fastest parts of the machine (registers, L1 cache) and avoids round-trips to the slow warehouse (main memory).

---

### Heuristics for a "Good" Schedule (Signs of Health)

#### 1. Deep Nesting & Fusion (The #1 Goal)
*   **What it is:** The pipeline stages are computed inside the loops of their consumers.
*   **Why it's good:** This is the most important principle. It means data is passed between stages via ultra-fast CPU registers, completely avoiding slow main memory. This is **fusion**.
*   **What to look for in the CEFD:**
    *   **Deep indentation.** The diagram should look like a deep tree, not a flat list.
    *   The `S: (fused)` attribute is the ultimate sign of health.

#### 2. Coarse-Grained Parallelism
*   **What it is:** The outermost loop is parallelized, dividing the work into large, independent chunks for each CPU core.
*   **Why it's good:** This ensures all CPU cores are utilized efficiently. Giving each core a large task minimizes the overhead of thread management.
*   **What to look for in the CEFD:** The topmost `LOOP` node has the `>>parallel` directive.

#### 3. Fine-Grained Vectorization
*   **What it is:** The innermost loop over the most memory-contiguous data (usually `x`) is vectorized.
*   **Why it's good:** This leverages the CPU's SIMD units to process multiple pixels (e.g., 8, 16, or 32) with a single instruction.
*   **What to look for in the CEFD:** The most deeply nested `LOOP` or `FUNC` node has a `>>vectorize` or `D: vectorize(x)` directive.

#### 4. Smart Tiling (Producer-Consumer Locality)
*   **What it is:** Data is processed in tiles or strips that are small enough to fit in the CPU's L1 or L2 cache.
*   **Why it's good:** When a producer creates a tile of data, that data is still "hot" in the cache when the consumer needs it moments later. This avoids a round-trip to main memory.
*   **What to look for in the CEFD:** A two-level `LOOP` structure from a `split`, like `yo` (outer) and `yi` (inner). The producer is computed at the inner level (`@ yi` or `@ x`), ensuring its data is fresh for the consumer in the same loop.

#### 5. **(Expanded)** Contiguous Memory Access
*   **What it is:** Ensuring that when a vectorized loop reads from memory, it reads a contiguous block of data.
*   **Why it's good:** Vector loads are fastest when they can grab one wide chunk of memory. "Gather" operations, which read from scattered locations, are much slower.
*   **What to look for in the CEFD:** The `D: reorder_storage` directive. This is a sign that the developer has thought about how the data is laid out in memory to match how it will be accessed.
*   **Example:**
    ```
    └─ FUNC deinterleaved @ yo | S: ->yo | D: reorder_storage(c,x,y)  # Store channels together for better vectorized access later
    ```

#### 6. **(Expanded)** Isolated Complexities
*   **What it is:** Identifying complex, reusable computations (like a big blur, a histogram, or a reduction) and deliberately materializing them.
*   **Why it's good:** Sometimes, breaking fusion is a *good* thing. If a `Func` is very expensive and its results are used by many different consumers, it's better to compute it once at a high level (`@ root` or `@ yo`) and store it, rather than recomputing it inside every consumer.
*   **What to look for in the CEFD:** A `FUNC` with `@ root | S: (root)` that is a known bottleneck (e.g., contains an `RDom`). The rationale note should justify why it's not fused.

---

### Heuristics for a "Bad" Schedule (Performance Killers) & Remedies

#### 1. The Waterfall (Broken Fusion)
*   **What it is:** Every stage is computed in full and stored in a temporary buffer before the next stage begins.
*   **Why it's bad:** This is the **#1 performance killer**. It maximizes memory bandwidth usage and cache misses.
*   **What to look for in the CEFD:**
    *   A **flat structure**. All `FUNC` nodes are peers at the same indentation level.
    *   Repeated `S: -><var>` attributes on every `FUNC`.
*   **Remedy:** **Increase locality.** Change the schedule from `compute_at(outer_loop)` to `compute_at(consumer_func, inner_loop)`.

#### 2. Redundant Computation
*   **What it is:** Computing the same value over and over again. This happens when a `Func` is computed too deep inside a loop nest, but its values don't actually depend on the inner loop variables.
*   **Why it's bad:** Wastes CPU cycles.
*   **What to look for in the CEFD:** A `FUNC` is nested deep inside a `LOOP` (e.g., `@ yi`), but its inputs (e.g., a `Param` or another `Func` computed at `@ yo`) don't change with `yi`.
*   **Remedy:** **Hoist the computation.** Move the schedule to a higher, outer loop (`compute_at(..., yo)` or `compute_root()`).

#### 3. Serial Execution (No Parallelism)
*   **What it is:** The schedule uses only a single CPU core.
*   **Why it's bad:** Leaves most of the CPU's processing power idle.
*   **What to look for in the CEFD:** The complete absence of a `>>parallel` directive on any `LOOP` node.
*   **Remedy:** **Introduce parallelism.** Find the outermost dimension that can be processed independently (usually `y`) and apply a `.parallel()` directive.

#### 4. Scalar Code (No Vectorization)
*   **What it is:** The code processes only one pixel at a time instead of using SIMD.
*   **Why it's bad:** Fails to use the most powerful arithmetic units on the CPU (often an 8x-16x slowdown).
*   **What to look for in the CEFD:** The absence of `>>vectorize` or `D: vectorize(x)` on the innermost loops or `Funcs`.
*   **Remedy:** **Vectorize the innermost loop.**

#### 5. **(Expanded)** Inappropriate `compute_root`
*   **What it is:** A `Func` is scheduled with `compute_root()` when it's only used in a small part of the pipeline.
*   **Why it's bad:** `compute_root()` forces Halide to allocate a buffer for the *entire* image. If that `Func` is an intermediate, this buffer can be huge, thrashing the cache and potentially exceeding memory limits. It's a "global waterfall."
*   **What to look for in the CEFD:** A `FUNC ... @ root | S: (root)` that isn't an input/output or a truly global, reusable result.
*   **Remedy:** **Restrict the scope.** Change `compute_root()` to `compute_at()` a loop that is just high enough to avoid redundant computation, but no higher (e.g., `compute_at(processed, yo)`).

#### 6. **(Expanded)** The "Tiny Tile" Trap
*   **What it is:** Making tiles or vector widths too small.
*   **Why it's bad:** Every loop has overhead. If you process the image in 2x2 tiles, you'll spend more time managing the loops than doing actual work. Likewise, a vector width of 2 on a machine that supports 16 is leaving performance on the table.
*   **What to look for in the CEFD:** Small numbers in the loop dimensions, e.g., `[strip:2]` or `D: vectorize(x, 2)`.
*   **Remedy:** **Increase tile/vector sizes.** Aim for tile sizes that fill L1/L2 cache (e.g., 32-256 KB) and vector widths that match the machine's natural vector size (`target.natural_vector_size(...)`).

#### 7. **(Expanded)** Ignoring the Target
*   **What it is:** Writing a generic schedule without considering the specific architecture.
*   **Why it's bad:** Modern CPUs have specialized instruction sets (e.g., `HVX` on Hexagon, `AVX512` on x86) that offer massive speedups if used. A schedule that doesn't account for these is suboptimal.
*   **What to look for in the CEFD:** The absence of target-specific directives (e.g., `.hexagon()`) when you know you're compiling for a specialized target.
*   **Remedy:** **Specialize the schedule.** Use `get_target().has_feature(...)` to add target-specific scheduling directives.

---

### How to Diagnose: Measure, Don't Guess

You can't optimize what you can't measure. Use Halide's powerful environment variables to diagnose these issues:

1.  **`HL_PROFILE=1`**: Runs the pipeline and prints a detailed report of the time spent in every single `Func`, including memory allocations. If a `Func` you thought was fused (`S: (fused)`) shows up in this report, your schedule is not doing what you think it is.
2.  **`HL_TRACE_FILE=<path>`**: Generates a detailed event trace that can be viewed with tools like `HalideTraceViz` to see the pipeline executing over time.
3.  **`HL_DEBUG_CODEGEN=1`**: This is the ultimate ground truth. It dumps the low-level psuedo-code that Halide is about to JIT compile. You can read it to see the final loop nests and confirm if `Func`s are inlined (fused) or if there are loops calling `compute_and_store_...` (waterfall).
