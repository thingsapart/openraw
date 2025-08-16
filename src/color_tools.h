#ifndef COLOR_TOOLS_H
#define COLOR_TOOLS_H

#include "Halide.h"
#include "HalideBuffer.h"
#include "process_options.h"

// --- Halide Pipeline Color Conversion Helpers ---
// These are generator-side helpers that build Halide expression trees.
// They are defined as inline functions in the header so that only the
// pipeline_generator (which includes Halide.h) needs to see their definitions.
// The final executables do not need to compile or link this code.
namespace HalideColor {
    using namespace Halide;
    using namespace Halide::ConciseCasts;

    namespace { // Anonymous namespace for internal helpers
        inline Expr lab_f(Expr t) {
            const float T = 0.008856451679035631f;
            // This is the critical bug fix. Using Halide::pow instead of fast_pow
            // is required for the numerical precision needed for color science,
            // especially for values of t very close to zero.
            return select(t > T, pow(t, 1.0f / 3.0f), (7.787037037037037f * t) + (16.0f / 116.0f));
        }

        inline Expr lab_f_inv(Expr t) {
            const float delta = 6.0f / 29.0f;
            return select(t > delta, t*t*t, 3.0f*delta*delta*(t - 16.0f/116.0f));
        }
    }

    inline Func linear_srgb_to_xyz(Func srgb, Var x, Var y, Var c) {
        Expr r = srgb(x, y, 0), g = srgb(x, y, 1), b = srgb(x, y, 2);
        Expr X = 0.4124564f*r + 0.3575761f*g + 0.1804375f*b;
        Expr Y = 0.2126729f*r + 0.7151522f*g + 0.0721750f*b;
        Expr Z = 0.0193339f*r + 0.1191920f*g + 0.9503041f*b;
        Func xyz("linear_srgb_to_xyz");
        xyz(x, y, c) = mux(c, {X, Y, Z});
        return xyz;
    }

    inline Func xyz_to_lab(Func xyz, Var x, Var y, Var c) {
        const float Xn = 0.95047f, Yn = 1.0f, Zn = 1.08883f;
        Expr fX = lab_f(xyz(x, y, 0)/Xn), fY = lab_f(xyz(x, y, 1)/Yn), fZ = lab_f(xyz(x, y, 2)/Zn);
        Expr L = 116.0f*fY - 16.0f, a = 500.0f*(fX - fY), b = 200.0f*(fY - fZ);
        Func lab("xyz_to_lab");
        lab(x, y, c) = mux(c, {L, a, b});
        return lab;
    }

    inline Func lab_to_lch(Func lab, Var x, Var y, Var c) {
        Expr L = lab(x, y, 0), a = lab(x, y, 1), b = lab(x, y, 2);
        Expr C = sqrt(a*a + b*b);
        // Stabilize hue calculation for near-achromatic colors.
        Expr h = select(C > 1e-5f, atan2(b, a), 0.0f);
        Func lch("lab_to_lch");
        lch(x, y, c) = mux(c, {L, C, h});
        return lch;
    }

    inline Func lch_to_lab(Func lch, Var x, Var y, Var c) {
        Expr L = lch(x, y, 0), C = lch(x, y, 1), h = lch(x, y, 2);
        Expr a = C * cos(h);
        Expr b = C * sin(h);
        Func lab("lch_to_lab");
        lab(x, y, c) = mux(c, {L, a, b});
        return lab;
    }

    inline Func lab_to_xyz(Func lab, Var x, Var y, Var c) {
        const float Xn = 0.95047f, Yn = 1.0f, Zn = 1.08883f;
        Expr L = lab(x, y, 0), a = lab(x, y, 1), b = lab(x, y, 2);
        Expr fY = (L + 16.0f)/116.0f, fX = a/500.0f + fY, fZ = fY - b/200.0f;
        Expr X = lab_f_inv(fX)*Xn, Y = lab_f_inv(fY)*Yn, Z = lab_f_inv(fZ)*Zn;
        Func xyz("lab_to_xyz");
        xyz(x, y, c) = mux(c, {X, Y, Z});
        return xyz;
    }

    inline Func xyz_to_linear_srgb(Func xyz, Var x, Var y, Var c) {
        Expr X = xyz(x, y, 0), Y = xyz(x, y, 1), Z = xyz(x, y, 2);
        Expr r =  3.2404542f*X - 1.5371385f*Y - 0.4985314f*Z;
        Expr g = -0.9692660f*X + 1.8760108f*Y + 0.0415560f*Z;
        Expr b =  0.0556434f*X - 0.2040259f*Y + 1.0572252f*Z;
        Func srgb("xyz_to_linear_srgb");
        srgb(x, y, c) = mux(c, {r, g, b});
        return srgb;
    }

    inline Func linear_srgb_to_lch(Func srgb, Var x, Var y, Var c) {
        Func xyz = linear_srgb_to_xyz(srgb, x, y, c);
        Func lab = xyz_to_lab(xyz, x, y, c);
        Func lch = lab_to_lch(lab, x, y, c);
        xyz.compute_inline(); lab.compute_inline();
        return lch;
    }

    inline Func lch_to_linear_srgb(Func lch, Var x, Var y, Var c) {
        Func lab = lch_to_lab(lch, x, y, c);
        Func xyz = lab_to_xyz(lab, x, y, c);
        Func srgb = xyz_to_linear_srgb(xyz, x, y, c);
        lab.compute_inline(); xyz.compute_inline();
        return srgb;
    }
}


// --- Host-side Color Grading LUT Generation ---
// These are standard C++ functions for use by the host application (e.g. process, rawr).
// They only require HalideBuffer.h and the runtime.
namespace HostColor {
    // Generates a 3D LUT from the color parameters in the ProcessConfig.
    // The LUT maps input L*C*h* values to output L*C*h* values.
    // Dimensions are [L_in, C_in, h_in, 3], where the last dim is the output L'C'h' tuple.
    Halide::Runtime::Buffer<float, 4> generate_color_lut(const ProcessConfig& cfg, int size = 33);
}

#endif // COLOR_TOOLS_H

