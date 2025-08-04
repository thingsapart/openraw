
### 1. The Philosophy: A Canonical RAW Pipeline

The key to high-quality results is performing operations in the correct order and in the most appropriate color space. While a user can adjust sliders in any order, the internal processing must follow a strict, logical flow. This flow is generally divided into five main phases:

1.  **Pre-Demosaic (Sensor Data Correction):**
    *   **What it is:** Working directly on the single-channel, mosaiced Bayer data. This data is *linear*, meaning its values are directly proportional to the amount of light that hit the sensor.
    *   **Why it's first:** The goal here is to fix fundamental flaws of the sensor and lens *before* color is even created. Correcting noise or aberrations at this stage is far more effective and prevents them from being amplified and spread across the three color channels during demosaicing.

2.  **Demosaicing (Color Creation):**
    *   **What it is:** The critical step of interpolating the Bayer pattern into a full-color, 3-channel (RGB) image.
    *   **Why it's a distinct step:** This is the point of transition from a single-channel to a multi-channel representation. The quality of this algorithm has a massive impact on fine detail, artifacts, and color moiré.

3.  **Linear Color Space Adjustments (Scene-Referred):**
    *   **What it is:** Operations on the full-color RGB image while the data is still in a *scene-referred*, linear state. This means the RGB values still have a direct physical relationship to the light in the original scene.
    *   **Why it's important:** White balance, exposure, and capture sharpening are physically-based corrections. Applying them in linear space produces natural, predictable results that don't create ugly artifacts like color shifts or halos around edges.

4.  **Tonal & Color Mapping (Display-Referred):**
    *   **What it is:** The transition from the physically linear scene-referred space to a non-linear, perceptual space that is suitable for viewing on a screen. This is where most "artistic" or "creative" adjustments happen.
    *   **Why it's separate:** This stage is about aesthetics, not physical correctness. Operations like tone curves, contrast, clarity, and film simulation are designed to make the image look subjectively pleasing. Performing them after the corrective work in linear space ensures they have a clean foundation to build upon.

5.  **Output & Finalization:**
    *   **What it is:** The final steps tailored for the specific output medium (e.g., a JPEG for the web, a TIFF for printing).
    *   **Why it's last:** Resizing and output sharpening must be the final operations. Sharpening, for instance, should compensate for the softness introduced by resizing and the viewing medium. Doing it earlier would result in incorrect sharpening.

---

### 2. Feature Deep Dive & Implementation Strategy

Let's place your requested features into this canonical pipeline and discuss their implementation in Halide.

#### Phase 1: Pre-Demosaic (On Bayer Data)

*   **Raw Black Point / White Point**:
    *   **Purpose**: Calibrates the raw sensor data. Black point subtracts the sensor's electronic bias. White point defines the clipping/saturation level.
    *   **Placement**: The very first step. Your current implementation is correct.
    *   **Halide Strategy**: A simple point-wise operation (`(raw - black) / (white - black)` for float, or `u16_sat(raw_signed - black)` for integer). Perfectly fusible.

*   **Noise Reduction (Raw NR)**:
    *   **Purpose**: Reduces sensor noise before it gets smeared into color artifacts by demosaicing.
    *   **Comparison**: High-end tools like **DxO PhotoLab (DeepPRIME)** and **Darktable ("raw denoise")** excel here.
    *   **Halide Strategy**: Can range from simple (your current hot pixel filter) to advanced (Non-Local Means, BM3D). These are stencil operations. Operating on single-channel raw data is a huge performance win.

*   **Defringe**:
    *   **Purpose**: Removes lateral chromatic aberration that appears as purple or green halos around high-contrast edges.
    *   **Comparison**: A key feature in **Lightroom** and **RawTherapee**.
    *   **Halide Strategy**: A neighborhood operation. You'd detect unnaturally high-frequency patterns in the Bayer data (e.g., a black pixel next to a clipped one) and replace/desaturate the offending pixels.

#### Phase 2: Demosaicing

*   Your modular approach using a `DemosaicDispatcherT` is state-of-the-art and a perfect use of Halide's capabilities. It matches the flexibility offered by **RawTherapee** and **Darktable**.

#### Phase 3: Linear Color Space Adjustments (Scene-Referred)

*   **White Balance**:
    *   **Purpose**: To neutralize color casts from ambient light by applying per-channel multipliers.
    *   **Placement**: The first step after demosaicing, before any other color work. Your current `color_temp` and `tint` approach is standard.
    *   **Halide Strategy**: A point-wise multiplication on each channel. Trivial to fuse.

*   **Lens Geo Fixing (Distortion Correction)**:
    *   **Purpose**: Corrects for barrel or pincushion distortion.
    *   **Comparison**: All major editors use lens profiles (often from the **Lensfun** database) to apply a polynomial warp.
    *   **Halide Strategy**: Halide is excellent at warps. The operation is `output(x, y) = input(distort_x(x, y), distort_y(x, y))`. The distortion functions are polynomials based on the distance from the image center.

*   **Exposure**:
    *   **Purpose**: A global brightness adjustment that simulates changing camera exposure.
    *   **Placement**: Must be done in linear space.
    *   **Halide Strategy**: A simple point-wise multiplication across all three channels: `rgb * pow(2.0f, exposure_stops)`.

*   **Capture Sharpening**:
    *   **Purpose**: To counteract the blurring from the lens and the camera's optical low-pass filter (OLPF). This is about restoring detail, not creative sharpening.
    *   **Comparison**: **RawTherapee's Richardson-Lucy Deconvolution** is a very advanced form of this. A simpler approach is a multi-radius unsharp mask.
    *   **Halide Strategy**: Your current unsharp mask is a good starting point. Applying it here in linear space prevents the bright/dark halos that occur when sharpening after a tone curve.

#### Phase 4: Tonal & Color Mapping (Display-Referred)

*   **Curves (Luma / RGB)**:
    *   **Purpose**: The primary tool for adjusting global contrast and tone.
    *   **Placement**: This is typically the first step in the display-referred stage, as it maps the linear data into a perceptual range. Your LUT-based implementation is highly efficient.

*   **Tonal Range (Blacks, Shadows, Highlights, Whites) & Local Contrast/Clarity**:
    *   **Purpose**: Intuitive sliders for adjusting specific tonal regions without complex curves. Clarity adds "punch" to mid-tone details.
    *   **Comparison**: A defining feature of **Lightroom**. **Darktable's "Tone Equalizer"** is a very powerful, mask-based alternative.
    *   **Halide Strategy**: The best way to implement this is with a base/detail separation. A **Guided Filter** or a large-radius bilateral filter can create a "base" layer (tones). The "detail" layer is `input - base`. Clarity boosts the detail layer's contrast. Shadows/Highlights adjustments are applied to the base layer using masks derived from the base layer's brightness.

*   **Vibrance**:
    *   **Purpose**: A "smart" saturation tool that primarily boosts less-saturated colors, often protecting skin tones.
    *   **Halide Strategy**: A point-wise operation. Convert RGB -> HSL, apply a non-linear gain to the Saturation channel, and convert HSL -> RGB. The entire chain can be fused.

*   **Film Simulation**:
    *   **Purpose**: Emulate the color and tonal response of classic film stocks.
    *   **Comparison**: Implemented in **Darktable ("lut 3D")** and **RawTherapee ("Film Simulation")** using lookup tables.
    *   **Halide Strategy**: Best implemented with a **3D LUT**. This is a `Buffer<float, 4>` (e.g., 33x33x33x3) where the input R, G, B values are used as coordinates to look up the output R, G, B values, with trilinear interpolation.

*   **B&W Conversion**:
    *   **Purpose**: Convert the image to monochrome.
    *   **Comparison**: Pro tools offer a "channel mixer" to control how the R, G, and B channels contribute to the final luminance, simulating the use of color filters in B&W photography.
    *   **Halide Strategy**: A point-wise operation: `luma = r*r_weight + g*g_weight + b*b_weight`.

#### Phase 5: Output & Finalization

*   **Crop**:
    *   **Purpose**: Select a sub-region of the image.
    *   **Halide Strategy**: This is a crucial optimization. Cropping is not a processing stage. It should be implemented by setting the bounds of the final output buffer. Halide's bounds inference will automatically propagate this request backward, ensuring no work is done on pixels outside the crop. This is a "free" operation that dramatically speeds up processing.

*   **Resize**:
    *   **Purpose**: Scale the image to final output dimensions.
    *   **Halide Strategy**: A warp operation. `output(x, y) = input(x * scale, y * scale)` with Halide handling the interpolation (e.g., bilinear).

*   **Screen Sharpening**:
    *   **Purpose**: A final, subtle sharpening to compensate for resizing softness.
    *   **Halide Strategy**: A small-radius unsharp mask applied as the very last step before casting to `uint8_t`.

---

### 3. Proposed Pipeline Structure & Roadmap

This translates the above into a concrete plan.

**Proposed Order of `Func`s:**

```
// --- Inputs ---
raw_input(x, y)
// ... params ...

// === Phase 1: Pre-Demosaic (Linear Sensor Data) ===
1. normalized_raw(x, y)         // Black subtraction & normalization
2. raw_denoised(x, y)           // Raw noise reduction
3. defringed(x, y)              // Purple/green fringing removal

// === Phase 2: Demosaic ===
4. demosaiced(x, y, c)          // Bayer -> RGB

// === Phase 3: Linear Color Space (Scene-Referred RGB) ===
5. white_balanced(x, y, c)      // Apply WB multipliers
6. lens_corrected(x, y, c)      // Geometric distortion warp
7. exposure_adjusted(x, y, c)   // Exposure multiplication
8. capture_sharpened(x, y, c)   // Deconvolution or unsharp mask

// === Phase 4: Tonal & Color Mapping (Display-Referred RGB) ===
9. color_corrected(x, y, c)     // Camera profile color matrix (Your current 'corrected')
10. separated(x, y, c)          // Base/Detail separation (e.g., via Guided Filter)
11. base_adjusted(x, y, c)      // Shadows/Highlights on the 'base' layer
12. detail_adjusted(x, y, c)    // Clarity/Local Contrast on the 'detail' layer
13. recombined(x, y, c)         // base_adjusted + detail_adjusted
14. film_sim(x, y, c)           // Apply 3D LUT
15. vibrance(x, y, c)           // HSL-based smart saturation
16. tone_curved(x, y, c)        // Final tone curve (your current 'curved')
17. final_color(x, y, c)        // Optional B&W conversion mixer

// === Phase 5: Output ===
18. resized(x, y, c)            // Resampling to output size
19. output_sharpened(x, y, c)   // Final sharpening pass
20. final_u8(x, y, c)           // Cast to uint8_t
```

**Implementation Roadmap (Priority-Based):**

This roadmap provides a logical order for adding the new features.

**Priority 1: Foundational Controls** (Relatively easy, high impact)
1.  **Exposure**: Add a simple multiplier in linear space.
2.  **Crop**: Implement by setting output buffer bounds in `process.cpp`. No pipeline change needed.
3.  **Blacks/Whites/Contrast**: Your `tone_curve_utils` already handles contrast. Expose "Blacks" and "Whites" sliders that shift/scale the endpoints of the curve.
4.  **B&W Conversion**: Add a simple monochrome mode with a channel mixer.

**Priority 2: Core "Pro" Features** (Medium difficulty, key for quality)
1.  **Local Contrast / Clarity**: Implement the base/detail separation using your `stage_local_tonal_adjustments.h` as a starting point. This will also be the foundation for the Shadows/Highlights sliders.
2.  **Shadows / Highlights**: Using the "base" layer from the Clarity step, apply masked adjustments to brighten shadows and darken highlights.
3.  **Lens Distortion Correction**: Integrate a polynomial warp based on coefficients (you can hardcode them for your lens initially).
4.  **Chroma Noise Reduction**: Implement a YCbCr -> blur chroma -> RGB sub-pipeline.

**Priority 3: Advanced Features** (High difficulty or specialized)
1.  **Film Simulation (3D LUTs)**: Add support for loading `.cube` files and applying them via trilinear interpolation in Halide.
2.  **Raw Noise Reduction**: Research and implement a more advanced algorithm like Non-Local Means.
3.  **Defringe**: Implement a pre-demosaic fringe detection and correction algorithm.

