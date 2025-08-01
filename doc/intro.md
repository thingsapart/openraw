I have a raspberry pi with an IMX585 sensor, but potentially other sensors like IMX294, in both monochrome and color versions - camera can use either.

The goal is to create an open-source, relatively high-quality camera system - some of the quality is sensor-limited but computational photography could potentially alleviate some of this.

Now, we're concerned with the software part. I'd like to create an "image pipeline" that can take the raw sensor data as captured by libcamera (ideally in as high bit-depth as possible) and turns it into various "consumer image formats" or DNG raw files.

Goals of the whole system for reference (same codebase, potentially different "incarnations") - but i think the main guiding principle is "high-quality output in a fun package".

Software:

1. Customizable "look":
  * rich film effects and simulations (eg using 3D LUTs),
  * curves for highlights, midtones and lows,
  * potentially tone-mapping,
  * various methods for clarity and sharpening, local laplacian filters,
  * other methods of achieving looks (which?)
2. In-camera raw-editing:
  * color-calibrating the built-in camera LCD,
  * live-adjustment of most basic settings that lightroom would offers, but especially:
    - sharpening and clarity,
    - curves,
    - colors
    - save these settings for later editing,
3. high-quality output in a fun package
  * maybe later pixel-shift, multiple exposures for sharpness or exposure blending HDR, ... (suggest more!).
4. Manual aides: light metering + visualizations, no AF but manual focus aides.


