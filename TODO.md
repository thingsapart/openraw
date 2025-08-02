# Pipeline Feature Roadmap

A list of potential features to add to the RAW processing pipeline, sorted by priority (a mix of user impact and implementation difficulty).

## Priority 1: High-Impact & Easier to Implement

These are fundamental adjustments that are missing and provide the most value for the effort.

- [x] **Exposure Control:** Add a global multiplier to brighten or darken the image in linear space before the tone curve.
- [x] **Global Saturation:** Add a master saturation control to increase or decrease the intensity of all colors. Implemented with two selectable algorithms: HSL and L*a*b*.
- [x] **Blacks & Whites Sliders:** Expose controls to adjust the black and white clip points of the tone curve. (Integrated into Curve stage).
- [x] **Custom Tone Curve:** Implement a flexible tone curve using user-defined control points, applied to either Luma or RGB, with optional per-channel overrides and a PNG visualization.
- [ ] **Vignette Correction:** Implement a basic radial darkening or brightening to counteract or add a lens vignette effect.
- [ ] **Crop Tool:** Add parameters to define a crop rectangle, effectively changing the output bounds of the pipeline.

## Priority 2: Core Features & Harder to Implement

These features are central to modern RAW editing but require more complex algorithms or data handling.

- [ ] **Highlights & Shadows Recovery:** Implement a non-linear adjustment to specifically recover detail in the brightest and darkest parts of the image.
- [ ] **Clarity / Local Contrast:** Add a local contrast enhancement filter (e.g., using a large-radius unsharp mask) to add "punch" to mid-tones.
- [ ] **Color (Chroma) Noise Reduction:** Implement a filter to reduce random color blotches, typically by blurring the color channels in a perceptual color space (e.g., YCbCr, L*a*b*).
- [ ] **HSL Color Panel:** Create a system to adjust the Hue, Saturation, and Luminance of specific color ranges (Reds, Greens, Blues, etc.).
- [ ] **Lens Distortion Correction:** Implement a polynomial transform to correct for geometric barrel or pincushion distortion from lenses.
- [ ] **Split Toning:** Add controls to apply separate color tints to the shadows and highlights of the image.

## Priority 3: Advanced, Niche, or Architecturally Complex

These features are powerful but are very difficult to implement or are for more specialized use cases.

- [x] **Advanced Demosaicing Algorithms:** Provide options for different demosaicing algorithms (e.g., VHG) to allow users to trade between detail and artifacts.
- [ ] **Local Adjustments (Brushes, Gradients):** Architect a system for applying adjustments via user-defined masks, which is a major departure from a global pipeline.
- [ ] **Input Color Profile (DCP/ICC) Support:** Add the capability to parse standard camera profile files for more accurate color reproduction.
- [ ] **Perspective Correction:** Implement a full projective transform to correct for geometric keystoning.
- [ ] **LUT File Support:** Add the ability to load and apply 3D Look-Up Tables (e.g., from `.cube` files) for creative color grading.
- [ ] **Dehaze:** Implement a specialized algorithm to remove or add atmospheric haze by analyzing local color and contrast.

