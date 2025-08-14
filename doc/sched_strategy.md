### High-Level Scheduling Strategy

The pipeline is scheduled around the `final_stage`, which is computed in parallel strips (`yo_f`) composed of smaller tiles (`xo_f`).

1.  **Strip Level (`yo_f`):** This is the coarse-grained parallel loop. Any `Func` that needs to provide a "sliding window" of data to a consumer in the next tile *must* have its storage allocated at this level. This applies to the entire front-end of the pipeline (from `denoised_raw` to `sharpened`) and the base of the local wavelet pyramid. It also applies to the horizontal passes (`_blur_x`) of all separable blurs.
2.  **Tile Level (`xo_f`):** This is the fine-grained inner loop. Point-wise operations and `Func`s that don't need a large vertical halo are computed here. This maximizes data locality (fusion), ensuring data stays in the cache between stages within a tile. This applies to the final reconstruction part of the wavelet stage and the tone curve application.
3.  **Root Level:** This is reserved *only* for small, truly global computations that are independent of the main image tiling. This includes the camera matrices, sharpening kernel, and the entire global wavelet pyramid (which is computed from a small preview image).


