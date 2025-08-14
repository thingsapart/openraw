### The Core Analogy: The Assembly Line

Imagine you are building a car on an assembly line. Each stage (chassis, engine, paint) is a `Func` in your Halide pipeline.

*   **A "Strip Waterfall"** is like having a separate, massive warehouse for each stage. The chassis team builds 32 car chassis and puts them all in Warehouse A. Then, the engine team takes all 32 chassis from Warehouse A, installs engines in them, and puts them all in Warehouse B. Finally, the paint team takes all 32 cars from Warehouse B and paints them. This works, but it requires huge warehouses and a lot of moving things around.

*   **A "Tiled Sliding Window"** is a modern, just-in-time assembly line. The chassis team builds *one* chassis and immediately passes it to the engine team. The engine team installs the engine and immediately passes it to the paint team. There are no large warehouses; the "buffer" is just the single car being passed down the line.

---

### 1. The "Strip Waterfall" Schedule

This is a common and often effective first step in optimizing a Halide pipeline. It provides good parallelism but can be very memory-intensive.

#### What it is:

The image is broken down into large, independent horizontal strips. Each CPU core is assigned a strip to process in parallel. **Crucially, within each strip, every stage of the pipeline computes its result for the *entire strip* and stores it in a temporary buffer before the next stage begins.**

#### The Halide Schedule:

This schedule is primarily defined by two directives:
1.  `.split(y, yo, yi, strip_size).parallel(yo)`: This creates the parallel strips. `yo` is the outer loop over strips, and `yi` is the inner loop over scanlines *within* a strip.
2.  `producer.compute_at(consumer, yo)`: This is the key. By computing the producer at the **outer loop (`yo`)**, you are telling Halide: "Before you start the `yo` loop for the consumer, please compute and store the *entire strip* for the producer."

```cpp
// A simplified Strip Waterfall schedule
final_stage
    .split(y, yo, yi, 32) // Create 32-line strips
    .parallel(yo);        // Process strips in parallel

// Each of these stages will allocate a full 32-line buffer per thread.
denoised.compute_at(final_stage, yo);
demosaiced.compute_at(final_stage, yo);
sharpened.compute_at(final_stage, yo);
curved.compute_at(final_stage, yo);
```

#### How it Executes (The "Waterfall"):

Let's visualize the execution for a **single thread** working on a single 32-line strip:



1.  **Allocate `denoised` buffer:** A large buffer is allocated, big enough to hold the entire 32-line strip for `denoised`.
2.  **Compute `denoised`:** The `denoised` `Func` runs and completely fills this buffer.
3.  **Allocate `demosaiced` buffer:** A second large buffer is allocated for `demosaiced`.
4.  **Compute `demosaiced`:** The `demosaiced` `Func` reads from the `denoised` buffer and fills its own buffer. The `denoised` buffer can now be freed.
5.  **Repeat:** This process continues, creating a "waterfall" of large, temporary, strip-sized allocations.

#### Key Characteristics:

*   **Pros:**
    *   Simple to write and reason about.
    *   Achieves good coarse-grained parallelism.
*   **Cons:**
    *   **High Peak Memory Usage:** The peak memory is determined by the size of the largest intermediate strip buffer, multiplied by the number of parallel threads. This is exactly what caused your 190MB memory problem.
    *   **Poor Cache Locality:** Data produced by one stage (e.g., `denoised`) is likely "cold" (evicted from the fast CPU L1/L2 cache) by the time the next stage (`demosaiced`) reads it from main memory.

---

### 2. The "Tiled Sliding Window" Schedule

This is the advanced, memory-miserly evolution of the strip waterfall. It maximizes data reuse by keeping intermediate results in the fastest possible memory (CPU cache).

#### What it is:

The image is still broken into parallel strips. However, **within each strip, data flows between stages in tiny chunks (e.g., a few scanlines at a time) using small, efficient circular buffers.** No stage ever computes or stores its result for the entire strip at once.

#### The Halide Schedule:

This schedule uses a trio of directives to create the sliding window:
1.  `producer.store_at(consumer, yo)`: Tells Halide that the producer's storage is *scoped* to the parallel strip.
2.  `producer.compute_at(consumer, yi)`: This is the critical change. It tells Halide to compute the producer inside the **inner loop (`yi`)**, fusing the producer and consumer. It computes values for the producer *just in time* as the consumer needs them.
3.  `producer.fold_storage(y, N)`: This is the masterstroke. It tells Halide not to allocate a full strip-sized buffer, but instead to use a tiny **circular buffer** of only `N` scanlines.

```cpp
// A simplified Tiled Sliding Window schedule
final_stage
    .split(y, yo, yi, 32)
    .parallel(yo);

// Each stage now uses a tiny circular buffer.
denoised.store_at(final_stage, yo).compute_at(final_stage, yi).fold_storage(y, 5);
demosaiced.store_at(final_stage, yo).compute_at(final_stage, yi).fold_storage(y, 4);
sharpened.store_at(final_stage, yo).compute_at(final_stage, yi).fold_storage(y, 3);
curved.compute_at(final_stage, yi); // Pointwise, needs no storage
```

#### How it Executes (The "Sliding Window"):

Let's visualize the execution for the **same thread** on the same strip:



1.  **Allocate Tiny Buffers:** At the start of the strip, small circular buffers are allocated for `denoised` (e.g., 5 lines), `demosaiced` (4 lines), and `sharpened` (3 lines).
2.  **Loop `yi` (Scanline 0):**
    *   `sharpened` needs its first line.
    *   This triggers `demosaiced` to compute a few lines.
    *   This triggers `denoised` to compute its first few lines.
    *   The data is computed and immediately consumed, staying hot in the CPU cache.
3.  **Loop `yi` (Scanline 1):**
    *   `sharpened` needs its next line.
    *   The producers compute only the *new* scanlines they need, storing them in their circular buffers and overwriting the oldest, no-longer-needed data.
4.  **Repeat:** The "window" of active computation slides down through the strip, maintaining only a tiny memory footprint at any given moment.

#### Key Characteristics:

*   **Pros:**
    *   **Minimal Memory Usage:** Peak memory is drastically reduced, as only small circular buffers are ever allocated. This is how we solve your 190MB problem.
    *   **Maximum Cache Locality (Producer-Consumer Fusion):** Data is passed between stages via the L1/L2 cache, which is orders of magnitude faster than main memory.
*   **Cons:**
    *   **More Complex Schedule:** The schedule is more intricate.
    *   **Stencil Analysis:** The size `N` of the `fold_storage` circular buffer is critical. It must be large enough to satisfy the *total cumulative vertical stencil* of all downstream stages. Getting this wrong leads to runtime errors, but as you've seen, Halide's error messages handily tell you the exact size required.

### Summary Table

| Feature                 | Strip Waterfall                                     | Tiled Sliding Window                                           |
| ----------------------- | --------------------------------------------------- | -------------------------------------------------------------- |
| **Analogy**             | Separate warehouses for each assembly line stage.     | A just-in-time assembly line with no warehouses.               |
| **Key Schedule**        | `producer.compute_at(consumer, yo)`                 | `producer.store_at(yo).compute_at(yi).fold_storage(y, N)`      |
| **Memory Allocation**   | Allocates a full **strip-sized buffer** per stage.  | Allocates a tiny **N-line circular buffer** per stage.         |
| **Memory Footprint**    | **High.** `(strip_width * strip_height * Bpp)`      | **Minimal.** `(strip_width * N * Bpp)`                         |
| **Cache Locality**      | **Poor.** Data often goes "cold" between stages.    | **Excellent.** Data stays in L1/L2 cache between stages.       |
| **Primary Use Case**    | Simple parallelism, prototyping, or when memory is not a constraint. | Memory-constrained environments, maximizing performance on CPUs. |
| **Your Pipeline State** | **The cause of the 190MB memory usage.**            | **The solution to the 190MB memory usage.**                    |
