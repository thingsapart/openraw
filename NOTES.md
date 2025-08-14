1.  **`specialize()` vs. Scheduling:** The core conflict we've been fighting is between a `Func`'s *schedule* and its *definition*. A `specialize(condition)` call is part of the schedule. It tells the compiler, "When you generate code for this `Func`, you can assume `condition` is true." This only works if the compiler can use that assumption to simplify the `Func`'s definition (the part with the `select` statement).

2.  **The Prover is Conservative:** The `Failed to prove...` messages from the debug log are the compiler's proof engine saying it cannot be 100% certain that your `specialize` condition makes the other branches of the `select` unreachable. Floating-point comparisons (`<=` vs `<`) and compound boolean logic (`&&`) make this proof harder. If the proof fails, the compiler conservatively keeps all the code branches, and the specialization has no effect.

3.  **`Expr` Identity is Paramount:** Your insight was critical. The debug log showing `v0` and `v1` instead of `denoise_strength` and `denoise_radius` proves that the compiler sees the `Expr`s in the `specialize()` call as different variables from the `Expr`s in the `Func`'s definition. The fix is to ensure the **exact same `Expr` object** is used in both places. Passing the `Input<>` parameters (which convert to `Expr`) into the builder's constructor was the correct way to solve this, even though it revealed the next problem.

4.  **`compute_root()` Isolates a `Func`:** When you schedule a `Func` with `compute_root()`, you are telling the compiler to generate the code for it as a completely separate, standalone kernel that runs before everything else. This means any `specialize()` calls that are part of the *downstream* pipeline's schedule cannot affect the already-compiled `compute_root()` kernel. This is a fundamental scheduling conflict.

5.  **Nested Logic Requires Nested Scheduling:** The most robust way to make the proof easy for the compiler is to make the schedule's structure perfectly mirror the definition's logic. If your `Func` is defined with a nested `select`, you should use nested specializations. `denoised_raw.specialize(A); denoised_raw.specialize(B);` creates two separate `else if` branches. In contrast, getting a `Stage` handle `s = denoised_raw.specialize(A)` and then calling `s.specialize(B)` creates a nested `if (A) { if (B) { ... } }` structure, which is what we need.

---

1.  **`select()` is an expression, `specialize()` is a schedule.** `select()` decides per-pixel at runtime; `specialize()` creates entirely separate, optimized code paths at compile time based on a runtime-constant condition.

2.  **The `select()` Predication Trap:** The compiler may compute **both** branches of a `select` before choosing one. This is a performance catastrophe if the branches are expensive `Func`s.

3.  **`specialize(condition)` is the cure for the Predication Trap.** It guarantees the condition is a compile-time constant within each generated code path, enabling true dead-code elimination.

4.  **`specialize()` requires a provable condition.** The Halide compiler's internal prover must be able to mathematically prove that the `condition` in your schedule makes other branches in your `Func`'s definition unreachable. If it can't, it will conservatively keep all code paths, and the specialization will fail silently (check the build log for "Failed to prove" warnings).

5.  **`Expr` identity is critical for the prover.** The `Expr` objects used in `specialize()` must be the *exact same IR nodes* as those in the `Func` definition. Pass `Input<>` parameters (which convert to `Expr`) into helpers to ensure identity. The compiler log showing generic variables like `v0` and `v1` instead of parameter names is a key symptom of this problem.

6.  **Nested `specialize()` mirrors nested logic.** To make proofs easy for the compiler, the schedule's structure must match the definition's logic. If a `Func` uses a nested `select`, use nested specialization. Get a `Stage` handle to an outer specialization and apply the inner specialization to that handle.

7.  **`compute_root()` breaks specialization from consumers.** A `Func` scheduled with `compute_root()` is compiled as a standalone kernel. `specialize()` calls made on that `Func` from a downstream part of the pipeline have no effect on its compilation. The specialization must be part of the `Func`'s own schedule, or the `Func` must be scheduled inside its consumer (e.g. `compute_at`).

8.  **A single, consistent graph is paramount for optimization.** All parts of your Halide pipeline must form one coherent graph for the compiler to analyze effectively.

9.  **Graph Integrity Pitfall #1: `Func` Name Collisions.** Reusing `Func` names in helpers (e.g., `"blur_x"`) creates a tangled spaghetti graph that silently kills optimizations. **Always generate unique names.**

10. **Graph Integrity Pitfall #2: `Var` Scope Mismatches.** Helpers must operate on the *exact same `Var` objects* as their input `Func`s. Pass `Var`s as arguments; never recreate them locally.

11. **Scheduling dependencies are transitive.** A `compute_root()` on a consumer forces all of its producers (and their producers, etc.) to also be computed at the root, which can break fusion unexpectedly. This can also lead to scheduling inversion errors if a producer is scheduled at a finer loop level than its `compute_root` consumer.

12. **Nested parallelism causes deadlocks.** A `Func` containing a reduction (`sum`) cannot be scheduled `compute_at` an inner loop (`yi`) if the outer loop (`yo`) is parallel.

13. **Fix deadlocks by moving computation up the loop nest.** Scheduling the problematic `Func` at `compute_at(..., yo)` computes it serially once per parallel strip, avoiding the deadlock.

14. **The Profiler (`HL_PROFILE=1`) is the ground truth for performance.** If an expensive `Func` is still in the trace, your optimization has not worked, regardless of what the code says.

15. **The Profiler only reports materialized `Func`s.** If a `Func` is missing from the profile, it was successfully fused. An empty profile can mean your pipeline is perfectly efficient.

16. **The Generator Log (`HL_DEBUG_CODEGEN=1`) is the ground truth for compilation.** "Injecting realization" means a `Func` is being materialized; "Inlining" means it's being fused.

17. **The Build System can lie by using cached artifacts.** An incomplete `DEPENDS` list in CMake's `add_custom_command` is a common cause of running old code despite source changes.

18. **Always do a clean build (`rm -rf build`) after fixing the build system.** This is the only way to guarantee you're not using a stale, cached library.

19. **`RDom` sizes must be compile-time constants (`int`).** To have a dynamic radius, you must create multiple `Func` versions and use `select` or `specialize` to choose between them.

20. **A `Func` is "live" if a reference to it exists anywhere.** Storing unused `Func`s in a C++ data structure (like a `std::vector`) can keep them alive and prevent dead-code elimination.

21. **Profiling requires a specific build configuration.** You must use a `GeneratorParam`, pass the flag in CMake, and link the final executable to the full `Halide::Runtime`.
