// Turns the upscaler's linear HDR output into the kind of picture Neural Rendering was trained on, and
// folds the model's answer back into the frame.
//
// Two passes:
//
//   ENCODE   linear HDR -> an sRGB-encoded picture in [0,1], plus an untouched copy of the frame. The
//            picture is what the model is shown; the copy is what the answer is folded back into.
//
//   RESOLVE  proxy + model output + that copy -> the finished frame.
//
// The first version of this decoded the model's output back through the inverse of the tone curve, and
// that is what turned every strip light in Cyberpunk into a string of coloured cells. Two reasons, both
// fatal on highlights:
//
//   * The curve was applied per channel, so a saturated bright light had its channels compressed by
//     different amounts and came back a different hue.
//
//   * x/(1-x) diverges as x approaches one. A light sitting at 0.99 in the encoded picture reconstructs
//     to a hundred times the white point, and the model nudging one channel by a thousandth moves that
//     by tens of percent. Highlights are exactly where the model has least to say and where the inverse
//     amplifies most, which is the worst possible combination.
//
// The resolve does not reconstruct the frame by inverting that encode. It keeps the original and
// rescales the model's own picture onto it, so nothing depends on the encode being invertible and a
// bright pixel is never reconstructed from a compressed one. At zero strength the frame is
// bit-for-bit what the upscaler produced.

#pragma once

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

namespace codec
{
constexpr int MODE_ENCODE = 0;
constexpr int MODE_RESOLVE = 1;
// Shrinks the frame so the model can work on fewer pixels. Filtered, not point sampled: the guidance is
// explicit that a nearest-neighbour enlargement of this pass turns into harsh aliasing.
constexpr int MODE_DOWNSAMPLE = 2;

// Carries the model's edit from the resolved image onto the game's jittered one, so a multi-pass chain
// can still end in a real Super Resolution pass. See the shader.
constexpr int MODE_TRANSFER = 3;

// Point downsample, for depth: a filtered depth tap that straddles a silhouette returns a distance
// where no surface is. gGuideWidth/gGuideHeight carry the SOURCE extent for this mode.
constexpr int MODE_POINT_DOWN = 4;
// Debug views, so the model's contribution can be looked at rather than guessed at.
constexpr int DEBUG_OFF = 0;
constexpr int DEBUG_PROXY = 1;      // the picture the model was shown
constexpr int DEBUG_MODEL = 2;      // the model's raw answer
constexpr int DEBUG_DIFFERENCE = 3; // what it changed, amplified

inline const char* kShaderSource = R"(
cbuffer Params : register(b0)
{
    uint  gMode;
    float gWhitePoint;
    uint  gWidth;
    uint  gHeight;
    float gTransferStrength;
    float gColourStrength;
    uint  gDebugView;
    float gMaxRatio;
    uint  gPassthrough;
    float gMvScaleX;     // motion vector units -> pixels of this dispatch
    float gMvScaleY;
    uint  gGuideWidth;   // the motion texture's valid region
    uint  gGuideHeight;
    // Where the resolved pair must be sampled so its ratio lands on the feature the jittered frame
    // actually holds at this pixel. Already signed by the host; zero disables the alignment.
    float gAlignX;
    float gAlignY;
    // How strongly to distrust the model's colour where it desaturated what it was shown. 0 disables.
    float gColourGuard;
    // The floor of the transferred ratio, as gMaxRatio is its ceiling. See the transfer.
    float gMinRatio;
    // Radius in pixels at which the model's edit is separated into tone and detail. See SplitEdit.
    float gDetailBand;
    // How much of each band survives. 1 and 1 reproduce the whole edit exactly, at any radius.
    float gToneStrength;
    float gDetailStrength;
    // Extra gain on the detail band, to hold the model's apparent strength constant across the
    // resolution it ran at and the enlargement applied afterwards. 1 leaves it alone.
    float gDetailScale;
    // The luminance band across which the transfer crosses from an additive edit to a multiplicative
    // one, as a fraction of the white point. Below gTransferLo it is entirely additive.
    float gTransferLo;
    float gTransferHi;
    // How completely the transferred edit fades out above the white point. 0 keeps it at full strength.
    float gHighlightDamping;
    // How much of the reprojected previous ratio field is blended in. 0 disables the accumulation.
    float gRatioHistory;
    // Set on the frame a scene transition lands, so nothing is carried across a cut.
    uint  gReset;
};

// How much colour a pixel has, independent of how bright it is: 0 is neutral, 1 fully saturated.
//
// Used to ask whether the model kept the chroma it was given. A ratio of two of these is meaningful
// where a difference of two colours is not, because the proxy and the model sit at different
// brightnesses by construction.
float Saturation(float3 c)
{
    const float hi = max(c.r, max(c.g, c.b));
    const float lo = min(c.r, min(c.g, c.b));
    return hi > 1e-6 ? (hi - lo) / hi : 0.0;
}

// Values the arithmetic below cannot reason about, bounded.
//
// A game's scene colour is an open-ended linear buffer, and a sun can arrive in it as infinity --
// R11G11B10_FLOAT, which is what Borderlands 4 hands its upscaler, represents Inf perfectly well. The
// normalisation below then computes rolled / displayLuma as 1 / Inf, which is zero, and multiplies
// that back into an infinite channel. Inf * 0 is NaN. The model is handed the NaN and what comes back
// is a block of garbage where the sun was.
//
// It matters more in the reordered arrangements than in post-process: those work on the game's own
// scene colour, where the sun is still at full intensity, rather than on an upscaler's output.
//
// A comparison rather than isfinite(): a NaN fails every comparison, so this replaces it with zero in
// the same expression that bounds an infinity, and behaves identically on cs_5_0 and cs_6_0.
static const float kSceneCeiling = 65504.0; // the largest finite half, which is what these buffers hold

float SanitizeChannel(float v) { return (v > 0.0) ? min(v, kSceneCeiling) : 0.0; }

float3 Sanitize(float3 c)
{
    return float3(SanitizeChannel(c.r), SanitizeChannel(c.g), SanitizeChannel(c.b));
}

// Colours outside the AP1 gamut are impossible on any display and read as sparkle where a bright
// saturated pixel is pushed further. Clamping inside AP1 and coming back keeps everything reachable.
float3 ClampAp1(float3 color)
{
    const float3x3 bt709_to_ap1 = { 0.613097, 0.339523, 0.047379,
                                    0.070194, 0.916354, 0.013452,
                                    0.020616, 0.109570, 0.869815 };
    const float3x3 ap1_to_bt709 = { 1.705051, -0.621792, -0.083259,
                                    -0.130256, 1.140805, -0.010548,
                                    -0.024003, -0.128969, 1.152972 };
    return mul(ap1_to_bt709, max(mul(bt709_to_ap1, color), float3(0.0, 0.0, 0.0)));
}

// ---------------------------------------------------------------------------------------------
// The composition below (UpgradeToneMap's two-branch ratio, the OkLab hue correction, and the blend
// between a luminance-only result and the model's own colour) is taken from RenoDX's DLSS 5 addon by
// clshortfuse -- https://github.com/clshortfuse/renodx. It is their design, not ours; see
// Licenses/RenoDX_LICENSE.txt. The OkLab matrices are Bjorn Ottosson's published constants and the
// AP1, sRGB and PQ transforms are standard colour science.
// ---------------------------------------------------------------------------------------------

// OkLab, so the model's colour can be reached without its hue being invented on the way. A ratio
// applied to an RGB triple does not move hue, but a difference added to one does -- which is what the
// old composition did, and why a warm subject could come back green. Here the result's chroma is
// rebuilt in the model's own hue direction and only its magnitude is taken from the scaled colour.
float3 CbrtSigned(float3 v) { return sign(v) * pow(abs(v), 1.0 / 3.0); }

float3 ToOkLab(float3 color)
{
    const float3x3 rgb_to_lms = { 0.4122214708, 0.5363325363, 0.0514459929,
                                  0.2119034982, 0.6806995451, 0.1073969566,
                                  0.0883024619, 0.2817188376, 0.6299787005 };
    const float3x3 lms_to_lab = { 0.2104542553, 0.7936177850, -0.0040720468,
                                  1.9779984951, -2.4285922050, 0.4505937099,
                                  0.0259040371, 0.7827717662, -0.8086757660 };
    return mul(lms_to_lab, CbrtSigned(mul(rgb_to_lms, color)));
}

float3 FromOkLab(float3 lab)
{
    const float3x3 lab_to_lms = { 1.0, 0.3963377774, 0.2158037573,
                                  1.0, -0.1055613458, -0.0638541728,
                                  1.0, -0.0894841775, -1.2914855480 };
    const float3x3 lms_to_rgb = { 4.0767416621, -3.3077115913, 0.2309699292,
                                  -1.2684380046, 2.6097574011, -0.3413193965,
                                  -0.0041960863, -0.7034186147, 1.7076147010 };
    float3 lms = mul(lab_to_lms, lab);
    return mul(lms_to_rgb, lms * lms * lms);
}

// Takes the hue and the chroma direction from `correct`, and only the chroma magnitude from
// `incorrect`. Scaling a colour by a luminance ratio changes how saturated it reads; this puts the
// saturation back where the model meant it without letting the hue drift.
float3 HueOkLab(float3 incorrect, float3 correct)
{
    float3 incorrectLab = ToOkLab(incorrect);
    const float3 correctLab = ToOkLab(correct);
    const float incorrectChroma = length(incorrectLab.yz);
    const float correctChroma = length(correctLab.yz);
    incorrectLab.yz = correctLab.yz * (correctChroma == 0.0 ? 1.0 : incorrectChroma / correctChroma);
    return ClampAp1(FromOkLab(incorrectLab));
}

Texture2D<float4>   gSource   : register(t0);  // encode: the frame. resolve: the proxy.
Texture2D<float4>   gModel    : register(t1);  // resolve: what the model returned.
Texture2D<float4>   gOriginal : register(t2);  // resolve: the untouched frame.
Texture2D<float4>   gMotion   : register(t3);  // the game's motion vectors, for reprojecting history.
Texture2D<float4>   gHistory  : register(t4);  // transfer: last frame's ratio field, in log2.
RWTexture2D<float4> gTarget   : register(u0);  // encode: the proxy. resolve: the frame.
RWTexture2D<float4> gKeep     : register(u1);  // encode: the untouched copy. transfer: this frame's ratio.
SamplerState        gLinear   : register(s0);  // so the edit can be read at a different size

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// The neighbourhood the tone/detail split is measured over. Tap 0 is the centre, so it doubles as
// the unfiltered value; the four corners at weight 1/8 make a tent rather than a box, which at these
// radii is indistinguishable in the result and cheaper to reason about at the edges.
static const float2 tapOffsets[5] = { float2(0.0, 0.0), float2(-1.0, -1.0), float2(1.0, -1.0),
                                      float2(-1.0, 1.0), float2(1.0, 1.0) };
static const float tapWeights[5] = { 0.5, 0.125, 0.125, 0.125, 0.125 };

// sRGB rather than a plain 2.2 power: it is what an SDR game buffer actually carries, and the model was
// trained on those.
float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055, step(0.0031308, v));
}

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), step(0.04045, v));
}

// A ratio brought inside [gMinRatio, gMaxRatio] by a curve rather than a cliff.
//
// A hard clamp is a discontinuity, and a pixel sitting near the bound crosses it as the sampled pair
// wobbles -- so the bound that exists to stop one pixel misbehaving becomes a source of its own
// frame-to-frame switching. Compressing toward the limit instead keeps the guarantee (nothing ever
// leaves the range) while making the approach to it smooth, so nothing snaps.
//
// Identity is preserved exactly at 1.0, which matters: that is what "the model changed nothing here"
// has to mean, and it is most of the frame.
float SoftLimit(float r)
{
    const float lo = min(gMinRatio, 1.0);
    const float hi = max(gMaxRatio, 1.0);

    if (r > 1.0)
    {
        const float headroom = hi - 1.0;
        return headroom > 1e-6 ? 1.0 + headroom * (1.0 - exp(-(r - 1.0) / headroom)) : 1.0;
    }

    const float floorroom = 1.0 - lo;
    return floorroom > 1e-6 ? 1.0 - floorroom * (1.0 - exp(-(1.0 - r) / floorroom)) : 1.0;
}

/*
 * Separates the model's edit into what it did to tone and what it did to detail, and reweights them.
 *
 * The two halves are different claims about the picture and it is entirely reasonable to want one
 * without the other. Tone is the low-frequency band -- light interaction, colour, the overall
 * verdict on how a surface should sit; detail is the high-frequency remainder, which is the material
 * and shading structure the model synthesises. Wanting the game's own tone back while keeping the
 * synthesised structure is not a compromise, it is a coherent preference, and until now there was no
 * way to express it: strength turned both down together.
 *
 * The split is done in log space because the edit is a ratio, and a ratio's natural decomposition is
 * a sum of logarithms rather than a sum of differences. That also makes it exact: at tone = 1 and
 * detail = 1 the two bands recombine into precisely the edit that went in, whatever the radius, so
 * the controls are free until somebody moves them.
 *
 * `scale` is separate from `detail` on purpose. It is not a preference but a correction -- see the
 * host, which derives it from the resolution the model ran at against the resolution its work is
 * finally displayed at.
 */
float SplitEdit(float full, float low, float tone, float detail, float scale)
{
    const float logFull = log2(max(full, 1e-6));
    const float logLow = log2(max(low, 1e-6));
    const float logHigh = logFull - logLow;

    return exp2(logLow * tone + logHigh * detail * scale);
}

// The edit at an arbitrary position, exactly as the resolve computes its own.
float3 EditAt(float2 uvq)
{
    float3 p = gSource.SampleLevel(gLinear, uvq, 0).rgb;
    float3 m = gModel.SampleLevel(gLinear, uvq, 0).rgb;

    if (gPassthrough == 0)
    {
        p = SrgbToLinear(p);
        m = SrgbToLinear(m);
    }

    return m - p;
}

)" R"(
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    // Normalised, so the source may be any size relative to this dispatch.
    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);

    /*
     * Mode 3 -- carry the model's edit from the resolved image onto the jittered one.
     *
     * This is what lets the multi-pass chain end in a real Super Resolution pass. The first pass
     * resolves the game's jitter so the model can work on a clean, antialiased frame, but that same
     * resolve is what leaves the enlargement with no subpixel variation to reconstruct from. Handing
     * the enlargement the resolved frame therefore costs it everything DLSS is good at.
     *
     * So the edit is measured here rather than delivered here: how much brighter or darker the model
     * made each pixel, as a ratio against what it was shown. That ratio is then applied to the game's
     * ORIGINAL jittered frame, which still carries its full subpixel content. The enlargement gets a
     * properly jittered image with the model's enhancement riding on it, and the game's real jitter
     * offsets to go with it.
     *
     * A luminance ratio, not a per-channel one. The edit is a brightness decision -- that is what the
     * composition established -- and applying it per channel would drag the jittered frame's hue
     * toward the resolved frame's, which is not a transfer of anything, just a second colour pass.
     *
     * gSource   what the model was shown  (the first pass's resolved output)
     * gModel    what it returned          (the same image, enhanced)
     * gOriginal the game's jittered frame (what the enlargement should actually see)
     */
    if (gMode == 3)
    {
        /*
         * Sampled at an offset, not loaded in place.
         *
         * The ratio is measured on the resolved frame and applied to the jittered one, and the same
         * scene feature does not sit at the same pixel in both -- the jittered frame sampled the scene
         * up to half a pixel away. Reading the pair straight off the grid therefore lands the model's
         * enhancement beside the feature it belongs to, and the enlargement's temporal accumulation
         * then averages that misplacement across frames with different offsets, which cancels the
         * enhancement rather than merely blurring it. The model's work arrives far weaker than it was.
         *
         * gAlign carries the offset that maps this pixel back to where the resolved pair holds the
         * same content, and the host now derives it from the game's own motion vector convention
         * rather than offering three signs to try: the programming guide states jitter offsets use
         * the same coordinate and direction system as motion vectors, which makes the direction a
         * consequence rather than a guess. Zero still restores the unaligned behaviour.
         */
        const float2 texel = 1.0 / float2(gWidth, gHeight);
        const float2 alignUv = (float2(id.xy) + 0.5 + float2(gAlignX, gAlignY)) * texel;

        /*
         * The edit, optionally low-passed before it is carried across.
         *
         * What survives this transfer is a tone decision, and tone is the low-frequency half of what
         * the model does. The high-frequency half -- structure -- cannot survive being measured on one
         * image and applied to another that sampled the scene half a pixel elsewhere, and worse, a
         * misplaced high-frequency ratio does not merely blur: the enlargement averages it across
         * frames with different offsets and cancels the enhancement outright, which is what makes the
         * transferred result read as barely doing anything.
         *
         * So the band is a control rather than an accident. A radius of zero collapses every tap onto
         * the centre and reproduces the unfiltered behaviour exactly, so there is one code path and no
         * branch; raising it surrenders structure and keeps tone, which is the right trade under Ray
         * Reconstruction in particular, where the structure band is the part that fights the denoise.
         *
         * Five taps in a tent, not a box: at the radii that are useful here the difference is
         * invisible and the cost is not.
         */
        /*
         * Each tap's own ratio, averaged -- not the ratio of the averages.
         *
         * These are not the same thing, and the difference is exactly where the flickering lives. A
         * quotient of two interpolated quantities is not the interpolation of their quotient, and the
         * gap between them grows with the gradient of what is being interpolated. On a flat surface
         * it is nothing. On a specular highlight, where the gradient is steepest and a subpixel shift
         * moves the sample a long way up the slope, it is large -- and because the alignment offset
         * follows the jitter, that error is redrawn every frame. High-contrast pixels then carry a
         * multiplier that changes frame to frame for no reason in the scene, the enlargement's
         * history logic sees samples that disagree, and highlights sparkle.
         *
         * Per-tap the quotient is taken while numerator and denominator still belong to the same
         * texel, and each one is bounded before it is averaged, so a single near-zero denominator can
         * no longer dominate the result for the whole neighbourhood.
         */
        float ratioLow = 0.0;
        float ratioFull = 1.0;
        float beforeLuma = 0.0;
        float afterLuma = 0.0;
        float3 lowBefore = float3(0.0, 0.0, 0.0);
        float3 lowAfter = float3(0.0, 0.0, 0.0);

        for (int t = 0; t < 5; ++t)
        {
            const float2 uvt = alignUv + tapOffsets[t] * gDetailBand * texel;
            const float3 p = Sanitize(gSource.SampleLevel(gLinear, uvt, 0).rgb);
            const float3 m = Sanitize(gModel.SampleLevel(gLinear, uvt, 0).rgb);

            const float pl = dot(p, kLuma);
            const float ml = dot(m, kLuma);

            beforeLuma += pl * tapWeights[t];
            afterLuma += ml * tapWeights[t];

            // The colours themselves, for the chroma the ratio cannot carry. Low-passed on purpose:
            // chroma is a low-frequency property and its high-frequency content here is measurement
            // noise from two differently-sampled images rather than anything the model decided.
            lowBefore += p * tapWeights[t];
            lowAfter += m * tapWeights[t];

            const float tapRatio = SoftLimit(pl > 1e-5 ? ml / pl : 1.0);
            ratioLow += tapRatio * tapWeights[t];

            // Tap 0 is the centre, so it is the unfiltered edit at this pixel.
            if (t == 0)
                ratioFull = tapRatio;
        }

        // Tone and detail weighted separately, and exactly recombined when both are 1.
        float ratio = SplitEdit(ratioFull, ratioLow, gToneStrength, gDetailStrength, gDetailScale);
        ratio = SoftLimit(ratio);

        /*
         * The ratio, averaged over time along the surface it belongs to.
         *
         * This is the ghosting fix, and it is worth being precise about why ghosting appears at all.
         * The enlargement is a temporal reconstructor: it accumulates samples of what it believes is
         * the same surface, and rejects history when the samples disagree. Feeding it the game's
         * jittered frame multiplied by a ratio is fine as long as that ratio is a property of the
         * SURFACE. It is not quite: the model re-decides a measurable part of its answer every frame,
         * and the alignment offset that positions the ratio moves with the jitter, so the same
         * surface arrives carrying a slightly different multiplier each frame. The enlargement reads
         * that as disagreement, and disagreement is what its history logic resolves by smearing.
         *
         * So the ratio is accumulated here instead, before it ever reaches that pass. The field is
         * surface-locked and low-frequency, which makes it far more forgiving under reprojection than
         * colour would be -- and the part that survives averaging is exactly the part that was a real
         * decision about the material, while the part that cancels is the per-frame churn.
         *
         * Doing it here rather than turning tone down is the point. Tone is the model's low-frequency
         * verdict on light and colour; it is a large part of what the model is FOR, and giving it up
         * to buy stability trades away the effect rather than fixing it.
         *
         * In log space, because the quantity being averaged is a ratio.
         */
        float logRatio = log2(max(ratio, 1e-6));

        if (gRatioHistory > 0.0 && gReset == 0)
        {
            // Where this surface was last frame. The scales arrive already converted to UV per motion
            // vector unit, so this is the same arithmetic whatever resolution the vectors are at.
            const float2 motion = gMotion.SampleLevel(gLinear, uv, 0).xy;
            const float2 histUv = uv + motion * float2(gMvScaleX, gMvScaleY);

            /*
             * Rejected where it cannot be trusted, by the same test a temporal antialiaser uses:
             * clamp the reprojected value into the range the current neighbourhood actually spans.
             * A disocclusion, a moving object, or a surface that has genuinely changed all produce a
             * history value outside that range, and clamping quietly discards it without needing a
             * depth test or a separate disocclusion pass.
             *
             * The five taps for that are the ones already gathered for the tone/detail split.
             */
            if (all(histUv > 0.0) && all(histUv < 1.0))
            {
                const float logLow = log2(max(ratioLow, 1e-6));
                const float logFull = log2(max(ratioFull, 1e-6));

                // A band around what this pixel and its neighbourhood are claiming, widened a little
                // so the clamp does not fight ordinary frame-to-frame variation and stall the average.
                const float lo = min(logLow, logFull) - 0.15;
                const float hi = max(logLow, logFull) + 0.15;

                const float logHistory = clamp(gHistory.SampleLevel(gLinear, histUv, 0).r, lo, hi);

                logRatio = lerp(logRatio, logHistory, saturate(gRatioHistory));
            }
        }

        // Kept for next frame BEFORE the highlight fade below, so what accumulates is the model's own
        // decision rather than a value already reduced for display.
        gKeep[id.xy] = float4(logRatio, 0.0, 0.0, 0.0);

        ratio = SoftLimit(exp2(logRatio));
)" R"(
        const float4 jittered = gOriginal.Load(int3(id.xy, 0));
        const float3 jit = Sanitize(jittered.rgb);
        const float jitLuma = dot(jit, kLuma);

        /*
         * Additive near black, multiplicative in the light.
         *
         * A ratio diverges as its denominator goes to zero, and the two frames disagree about which
         * pixels are near zero -- that is the entire point of transferring between them. So a pixel
         * the first pass resolved to almost nothing produces an enormous ratio, and that ratio then
         * multiplies a DIFFERENT almost-nothing in the jittered frame. The old floor at 1e-4 did not
         * fix this; it only moved where the blow-up started.
         *
         * An additive delta has no such failure: it is well defined at zero and says exactly what the
         * model said, which at that brightness is "this should be lifted by so much" rather than "by
         * so many times". In the highlights the reverse holds, and a ratio is what keeps a bright
         * surface's shape instead of flattening it. The crossover is a band rather than a threshold
         * so nothing draws an edge across a gradient that passes through it.
         *
         * kLuma sums to one, so adding the delta to all three channels moves the luminance by exactly
         * the delta -- the additive branch is a luminance statement too, not a channel one.
         */
        const float white = max(gWhitePoint, 1e-4);

        /*
         * The additive branch derived from the same ratio the multiplicative one uses, rather than
         * from the raw luminance difference.
         *
         * They agree exactly when nothing has been done to the ratio -- beforeLuma * (after/before -
         * 1) is after - before -- so this changes nothing on its own. It matters once the ratio has
         * been through the band split and the temporal average: taking the difference straight from
         * the samples would leave the additive branch carrying an unfiltered, unsplit edit, so the
         * two halves of the crossover would disagree and the seam between them would be visible on
         * exactly the dark gradients the crossover exists to protect.
         */
        const float delta = beforeLuma * (ratio - 1.0);

        const float band =
            smoothstep(gTransferLo * white, max(gTransferHi * white, gTransferLo * white + 1e-6), jitLuma);

        /*
         * The edit is faded out above the white point.
         *
         * Two reasons, and they point the same way. The model's opinion is least reliable there by
         * construction: the encode normalises luminance without bounding the individual channels, so
         * a pixel above white was shown to the model already clipped -- the same fact the colour
         * guard in the resolve exists to handle. And a multiplicative edit amplifies whatever
         * variation it carries in proportion to the value it multiplies, so any residual per-frame
         * wobble in the ratio is loudest at exactly the brightest pixels, which is what makes it read
         * as sparkle rather than as noise.
         *
         * Fading toward identity there costs the model its highlight work and buys back stability.
         * That is a real trade rather than a free fix, which is why it is a knob: gHighlightDamping
         * at 0 restores the unfaded behaviour exactly.
         */
        const float overWhite = saturate((jitLuma / white - 1.0) / 3.0);
        const float keep = 1.0 - overWhite * saturate(gHighlightDamping);

        float3 result = lerp(jit + (delta * keep).xxx, jit * lerp(1.0, ratio, keep), band);

        /*
         * The model's colour, which a brightness ratio cannot carry at all.
         *
         * This is most of why multi-pass reads weaker than post-process even when the model runs at
         * the same or a higher resolution. Post-process composes the model's whole picture -- its
         * light AND its colour, in its own hue -- onto the frame. The transfer measured a single
         * luminance ratio and threw the rest away, so everything the model decided about colour was
         * lost on the way to the enlargement. Tone and detail were being split out of half an edit.
         *
         * Carried as a CHANGE in chroma rather than as the model's absolute colour, which is what
         * makes it safe. The original objection to a per-channel ratio was right: that drags the
         * jittered frame's hue toward the resolved frame's, so a surface ends up wearing another
         * frame's colour rather than its own plus the model's opinion. A delta has no such effect --
         * where the model changed nothing, it adds nothing, exactly.
         *
         * Normalised by lightness before it is applied. OkLab's a and b scale with L, and the pair it
         * was measured on sits at a different brightness from the frame it lands on by construction,
         * so an absolute delta would over-saturate dark pixels and under-saturate bright ones.
         *
         * Gated on the same Colour strength the resolve uses, so the control means one thing in both
         * placements: at 0 the frame keeps the game's own hue exactly and only its light carries the
         * model's verdict.
         */
        if (gColourStrength > 0.0)
        {
            const float3 labBefore = ToOkLab(lowBefore);
            const float3 labAfter = ToOkLab(lowAfter);

            const float2 chromaBefore = labBefore.yz / max(labBefore.x, 1e-4);
            const float2 chromaAfter = labAfter.yz / max(labAfter.x, 1e-4);
            const float2 chromaDelta = chromaAfter - chromaBefore;

            float3 lab = ToOkLab(result);
            lab.yz += chromaDelta * lab.x * gColourStrength * keep;
            result = ClampAp1(FromOkLab(lab));
        }

        gTarget[id.xy] = float4(max(result, float3(0.0, 0.0, 0.0)), jittered.a);
        return;
    }

    /*
     * Mode 4 -- point downsample, for depth.
     *
     * Depth is not a colour and must not be filtered like one. A bilinear tap that straddles a
     * silhouette returns a distance where nothing is, and the upscaler reprojects against that ghost
     * geometry. Taking the nearest source texel keeps every value one the scene actually contains.
     */
    if (gMode == 4)
    {
        const int2 src = int2((float2(id.xy) + 0.5) * float2(gGuideWidth, gGuideHeight) /
                              float2(gWidth, gHeight));
        gTarget[id.xy] = gSource.Load(int3(src, 0));
        return;
    }

    if (gMode == 2)
    {
        gTarget[id.xy] = gSource.SampleLevel(gLinear, uv, 0);
        return;
    }

    if (gMode == 0)
    {
        float4 source = gSource.Load(int3(id.xy, 0));
        float3 frame = Sanitize(source.rgb);

        // Kept so the resolve has the frame as it was, rather than having to reconstruct it.
        gKeep[id.xy] = float4(frame, source.a);

        // Some games hand DLSS a frame that has already been through their tonemapper. The game says
        // which in its own DLSS creation flags, and converting one that needs no conversion is pure
        // damage, so it goes through untouched.
        if (gPassthrough != 0)
        {
            gTarget[id.xy] = float4(frame, source.a);
            return;
        }

        // What the model is shown. Mode 2 -- the default -- scales the frame and encodes it, and that
        // is all: the game is going to tone map this picture later, so tone mapping it here as well
        // shows the model a doubly compressed image. Measured against Cyberpunk's own numbers, the
        // Reinhard proxy handed the model a scene value of 1.0 as 0.55 and 1.5 as 0.64 -- flat, dark,
        // and nothing like the finished frame it was trained on. The model then synthesised weakly,
        // judged tone on a picture that does not exist, and its answer had to be un-crushed on the way
        // back. Mode 0 keeps that old curve, mode 1 the fitted one.
        float luma = dot(frame, kLuma);
        float3 display = frame / max(gWhitePoint, 1e-4);

        // A soft knee instead of a hard ceiling. Anything the curve leaves above 0.75 is rolled off
        // rather than clipped, so the model is never shown a field of flat white whose blown pixels
        // flip between frames -- unstable input is unstable output, and this is where a bright scene
        // would produce it.
        float displayLuma = dot(display, kLuma);

        if (displayLuma > 0.75)
        {
            float rolled = 0.75 + 0.25 * (1.0 - exp(-(displayLuma - 0.75) / 0.25));
            display *= rolled / displayLuma;
        }

        gTarget[id.xy] = float4(LinearToSrgb(display), source.a);
        return;
    }

    // Sampled rather than loaded: when the model ran at a reduced resolution these are smaller than the
    // frame, and its edit is enlarged here while the frame underneath stays untouched.
    float4 proxySample = gSource.SampleLevel(gLinear, uv, 0);
    float4 modelSample = gModel.SampleLevel(gLinear, uv, 0);

    // Nothing was encoded on the way in, so nothing is decoded here either.
    float3 proxy = Sanitize(gPassthrough != 0 ? proxySample.rgb : SrgbToLinear(proxySample.rgb));
    float3 model = Sanitize(gPassthrough != 0 ? modelSample.rgb : SrgbToLinear(modelSample.rgb));
    float4 originalSample = gOriginal.Load(int3(id.xy, 0));

    // All three pictures have to share a scale before their luminances can be compared. The proxy and
    // the model come back from an sRGB decode, so they sit in 0..1 where 1 is the white point; the
    // frame is raw linear and runs well past that. Comparing them unnormalised is a real bug and it
    // reads exactly like the model has stopped adding detail: with the frame several times larger,
    // the shadow branch never fires, every pixel takes the highlight branch, and the clamp flattens
    // the result to a near-constant scale. Colour still moves, because that comes from the model's
    // own hue, which is what makes the failure so confusing to look at.
    const float normScale = gPassthrough != 0 ? 1.0 : max(gWhitePoint, 1e-4);
    float3 original = Sanitize(originalSample.rgb) / normScale;

    float originalLuma = dot(original, kLuma);
    float proxyLuma = dot(proxy, kLuma);

    if (gDebugView == 1)
    {
        gTarget[id.xy] = float4(proxy * gWhitePoint, originalSample.a);
        return;
    }

    if (gDebugView == 2)
    {
        gTarget[id.xy] = float4(model * gWhitePoint, originalSample.a);
        return;
    }

    float3 edit = model - proxy;

    // Coring was tried here and removed: the per-frame churn's amplitude overlaps the real detail's,
    // so an amplitude threshold cannot separate them -- it only relocated the noise to the threshold.

    if (gDebugView == 3)
    {
        // Amplified and centred on grey, so both directions of the edit are visible at once.
        float3 shown = saturate(0.5 + edit * 20.0);
        gTarget[id.xy] = float4(SrgbToLinear(shown) * gWhitePoint, originalSample.a);
        return;
    }

    // The edit, averaged over time. The model re-decides a measurable fraction of its answer every
    // frame even on a static scene; blending each frame's edit with its own reprojected history keeps
    // the consistent part -- the detail -- and cancels the part that re-randomises. NVIDIA's own
    // motion vectors carry the history to where the surface is now.
)" R"(
    // The composition. The model's answer is not treated as a difference to add onto the frame -- it
    // is a complete picture in its own right, and it is brought back by rescaling it to sit where the
    // original's luminance says it should. Adding a difference is what let colour run away: nothing
    // bounded where the sum landed, so a warm subject could arrive green. Here both ends of every
    // blend are well-formed pictures, so everything between them is one too.
    float modelLuma = dot(model, kLuma);
    float3 upgraded;

    if (modelLuma <= 1e-5)
    {
        // The model can return an empty frame for an input it cannot read. Rescaling that collapses
        // the picture to black, so the frame is handed back untouched instead.
        upgraded = original;
    }
    else
    {
        float ratio;

        if (originalLuma < proxyLuma)
        {
            // Below what the proxy showed: the frame's own luminance is the target.
            ratio = originalLuma / max(proxyLuma, 1e-6);
        }
        else
        {
            // Above it, the difference is headroom the proxy could not represent -- brightness the
            // frame really has and the model never saw. It is handed back on top of the model's own
            // answer rather than scaled away, which is what kept highlights from being muted.
            ratio = (modelLuma + max(0.0, originalLuma - proxyLuma)) / modelLuma;
        }

        upgraded = lerp(original, HueOkLab(model * ratio, model), gTransferStrength);
    }

    // Detail strength decides how much of the model's picture is reached at all; colour strength
    // decides whether its colour comes with it. At 0 the frame keeps the game's own hue exactly and
    // only its light carries the model's verdict; at 1 the model's colour arrives as well.
    float upgradedLuma = dot(upgraded, kLuma);
    float lumaRatio = originalLuma > 1e-6 ? clamp(upgradedLuma / originalLuma, 0.0, gMaxRatio) : 1.0;

    /*
     * Where the model was shown a pixel it cannot represent, its colour there is not an opinion
     * worth taking.
     *
     * The encode normalises luminance but scales all three channels uniformly, so it bounds the luma
     * and not the channels. A bright saturated pixel therefore keeps a channel above 1.0 in the
     * proxy -- outside the display-referred range the model was trained on. It has no way to express
     * "green, brighter than white", so it returns near-white for that pixel, and the highlight branch
     * above then does its job correctly and carries that whiteness up to the frame's own luminance.
     * A neon light arrives white at full brightness.
     *
     * There is a design tension underneath this. The proxy is deliberately left bright enough to
     * clip, because the highlight branch is built on exactly that: originalLuma - proxyLuma is the
     * headroom the proxy could not represent, and dividing it away first leaves that branch nothing
     * to give back. So the clipping has to stay for luminance to come out right -- and the same
     * clipping is what destroys the chroma. This resolves the tension by keeping the clip and
     * declining only the colour damage it causes.
     *
     * Two conditions, and both are required:
     *
     *   clipped  how far the proxy left the model's range. Zero for anything inside it, which is
     *            almost the whole frame. This is the part that matters: where the model was working
     *            on a picture it could actually read, its colour is respected in full, including
     *            every deliberate shift of tone and light interaction it decided on. Judging by
     *            desaturation alone would have suppressed those too, and that is a real part of what
     *            the model does -- not something to throw away to fix a highlight.
     *
     *   kept     of the chroma it was given, how much came back. Only consulted where the pixel was
     *            out of range to begin with.
     *
     * A ratio rather than a difference, because the proxy and the model sit at different
     * brightnesses by construction and only the chroma fraction is comparable between them.
     *
     * None of this can fire in the passthrough path: an already tone-mapped frame is copied rather
     * than encoded, so the proxy is the original and nothing ever leaves the model's range. The
     * failure is specific to HDR because the encode is the only thing that creates it.
     */
    float colourStrength = gColourStrength;

    if (gColourGuard > 0.0 && gPassthrough == 0)
    {
        const float proxyPeak = max(proxy.r, max(proxy.g, proxy.b));

        // Ramped rather than switched: a hard test at 1.0 would draw a visible edge across a
        // gradient that crosses it.
        const float clipped = saturate((proxyPeak - 1.0) / 0.25);

        const float proxySat = Saturation(proxy);
        const float modelSat = Saturation(model);
        const float kept = proxySat > 1e-4 ? saturate(modelSat / proxySat) : 1.0;

        colourStrength *= lerp(1.0, kept, clipped * saturate(gColourGuard));
    }

    float3 result = lerp(original * lumaRatio, upgraded, colourStrength);

    /*
     * The same tone/detail separation the transfer does, applied here as a correction.
     *
     * The composition above has already decided the whole answer, so rather than rebuilding it per
     * band -- which would mean running the OkLab work five times -- the split is measured on the one
     * quantity that drives it, the model's luminance against the proxy's, and the result is scaled by
     * however much the reweighting would have changed that driver.
     *
     * Exact at tone = 1, detail = 1, scale = 1: the adjustment is then 2^0, and the frame is bit for
     * bit what it was before these controls existed.
     */
    if (gToneStrength != 1.0 || gDetailStrength != 1.0 || gDetailScale != 1.0)
    {
        float lowProxy = 0.0;
        float lowModel = 0.0;

        for (int rt = 0; rt < 5; ++rt)
        {
            const float2 uvr = uv + tapOffsets[rt] * gDetailBand / float2(gWidth, gHeight);
            float3 p = Sanitize(gSource.SampleLevel(gLinear, uvr, 0).rgb);
            float3 m = Sanitize(gModel.SampleLevel(gLinear, uvr, 0).rgb);

            if (gPassthrough == 0)
            {
                p = SrgbToLinear(p);
                m = SrgbToLinear(m);
            }

            lowProxy += dot(p, kLuma) * tapWeights[rt];
            lowModel += dot(m, kLuma) * tapWeights[rt];
        }

        const float fullEdit = proxyLuma > 1e-5 ? modelLuma / proxyLuma : 1.0;
        const float lowEdit = lowProxy > 1e-5 ? lowModel / lowProxy : 1.0;

        const float wanted = SplitEdit(fullEdit, lowEdit, gToneStrength, gDetailStrength, gDetailScale);
        const float had = SplitEdit(fullEdit, lowEdit, 1.0, 1.0, 1.0);

        result *= clamp(had > 1e-5 ? wanted / had : 1.0, min(gMinRatio, 1.0), max(gMaxRatio, 1.0));
    }

    // Back out of the normalised space the composition worked in.
    result *= normScale;

    gTarget[id.xy] = float4(max(result, float3(0.0, 0.0, 0.0)), originalSample.a);
}
)";

struct Params
{
    unsigned int mode;
    float whitePoint;
    unsigned int width;
    unsigned int height;
    float transferStrength;
    float colourStrength;
    unsigned int debugView;
    float maxRatio;
    // Set when the game's own buffer is already tone-mapped, in which case there is nothing to convert.
    unsigned int passthrough;
    // 0 off, 1 blend with the reprojected history, 2 restart the history.
    float mvScaleX;
    float mvScaleY;
    unsigned int guideWidth;
    unsigned int guideHeight;

    // Subpixel offset at which the transfer samples the resolved pair, already signed. See
    // MODE_TRANSFER: the ratio is computed on a resolved frame and applied to a jittered one, and the
    // same scene feature sits up to half a pixel apart in the two.
    float alignX;
    float alignY;

    // How strongly to distrust the model's colour where it desaturated what it was shown.
    //
    // The encode normalises luminance but not individual channels, so a bright saturated pixel keeps a
    // channel above 1.0 in the proxy -- outside anything the model was trained on. It cannot represent
    // "green, brighter than white", so it returns near-white there, and the highlight branch then
    // multiplies that whiteness up to the original's luminance. In HDR that turns neon lights white.
    //
    // 0 is the old behaviour. 1 falls back to a luminance-only edit exactly where the model lost the
    // chroma it was given, and changes nothing anywhere else.
    float colourGuard;

    // The floor of the transferred ratio. maxRatio is its ceiling; this bounds the other end, which
    // is where a near-black denominator used to send it.
    float minRatio;

    // Radius in pixels at which the model's edit is separated into tone and detail.
    float detailBand;

    // How much of each band survives. Tone is the low-frequency verdict -- light interaction and
    // colour; detail is the high-frequency remainder, the material and shading structure. At 1 and 1
    // they recombine into exactly the edit that went in, whatever the radius.
    float toneStrength;
    float detailStrength;

    // Extra gain on the detail band, correcting for the resolution the model ran at against the
    // resolution its work is finally displayed at. Not a preference; see the host.
    float detailScale;

    // Luminance band, as a fraction of the white point, across which the transfer crosses from an
    // additive edit to a multiplicative one.
    float transferLo;
    float transferHi;

    // How completely the transferred edit fades toward identity above the white point.
    //
    // The model was shown a clipped picture there -- the encode bounds luminance, not channels -- so
    // its opinion is least reliable exactly where a multiplicative edit amplifies most. 0 restores
    // the unfaded behaviour.
    float highlightDamping;

    // How much of the reprojected previous ratio field is blended into this one.
    //
    // The enlargement downstream is a temporal reconstructor, and it smears when the samples it
    // accumulates disagree. A ratio that changes frame to frame on a static surface -- because the
    // model re-decides part of its answer, and because the alignment offset moves with the jitter --
    // is exactly that disagreement. Averaging the field along the surface first removes the churn and
    // keeps the decision. 0 disables it.
    float ratioHistory;

    // Set on the frame a scene transition lands, so no history is carried across a cut.
    unsigned int reset;
};

static_assert(sizeof(Params) % 4 == 0, "root constants are dwords");
static_assert(sizeof(Params) / 4 <= 60, "root constants must leave room for the descriptor table");

// A typeless resource cannot be viewed, and the buffer the upscaler writes is occasionally declared that
// way, so the typed member of the same family is substituted.
inline DXGI_FORMAT TypedFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        // The sRGB view cannot be bound as a typed UAV, and the shader does its own transfer function.
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return f;
    }
}

// Owns the compute pipeline and the descriptors both passes need. The dispatches are recorded onto the
// caller's command list, so there is no queue or fence to manage here.
class Codec
{
  public:
    bool ensure(ID3D12Device* device)
    {
        if (pipeline_ != nullptr)
            return true;

        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;

        if (FAILED(D3DCompile(kShaderSource, strlen(kShaderSource), nullptr, nullptr, nullptr, "main",
                              "cs_5_1", 0, 0, &code, &errors)))
        {
            if (errors != nullptr)
                errors->Release();

            return false;
        }

        if (errors != nullptr)
            errors->Release();

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 5; // proxy, model, original, motion, previous edit
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2; // result, kept copy
        ranges[1].OffsetInDescriptorsFromTableStart = 5;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = sizeof(Params) / 4;

        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers = &sampler;

        ID3DBlob* serialized = nullptr;

        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr)))
        {
            code->Release();
            return false;
        }

        HRESULT hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                 IID_PPV_ARGS(&root_));
        serialized->Release();

        if (FAILED(hr))
        {
            code->Release();
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = root_;
        pso.CS.pShaderBytecode = code->GetBufferPointer();
        pso.CS.BytecodeLength = code->GetBufferSize();
        hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline_));
        code->Release();

        if (FAILED(hr))
            return false;

        // Five descriptors per dispatch, two dispatches a frame; a ring of eight keeps a frame's
        // descriptors from being overwritten while it is still in flight.
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = kRingSlots * kPerDispatch;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap_))))
            return false;

        stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device_ = device;
        return true;
    }

    // Every texture must already be in the state its slot needs: sources shader-readable, targets
    // writable. Slots a pass does not read still have to be populated, or the descriptor is undefined.
    void dispatch(ID3D12GraphicsCommandList* cmd, const Params& constants, ID3D12Resource* source,
                  ID3D12Resource* model, ID3D12Resource* original, ID3D12Resource* target,
                  ID3D12Resource* keep, ID3D12Resource* motion = nullptr,
                  ID3D12Resource* prevEdit = nullptr)
    {
        if (pipeline_ == nullptr)
            return;

        const unsigned int slot = ring_;
        ring_ = (ring_ + 1) % kRingSlots;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += (SIZE_T) slot * kPerDispatch * stride_;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += (UINT64) slot * kPerDispatch * stride_;

        ID3D12Resource* srvs[5] = { source, model != nullptr ? model : source,
                                    original != nullptr ? original : source,
                                    motion != nullptr ? motion : source,
                                    prevEdit != nullptr ? prevEdit : source };

        for (int i = 0; i < 5; ++i)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            srv.Format = TypedFormat(srvs[i]->GetDesc().Format);

            D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu;
            handle.ptr += (SIZE_T) i * stride_;
            device_->CreateShaderResourceView(srvs[i], &srv, handle);
        }

        ID3D12Resource* uavs[2] = { target, keep != nullptr ? keep : target };

        for (int i = 0; i < 2; ++i)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Format = TypedFormat(uavs[i]->GetDesc().Format);

            D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu;
            handle.ptr += (SIZE_T) (5 + i) * stride_;
            device_->CreateUnorderedAccessView(uavs[i], nullptr, &uav, handle);
        }

        ID3D12DescriptorHeap* heaps[] = { heap_ };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetComputeRootSignature(root_);
        cmd->SetPipelineState(pipeline_);
        cmd->SetComputeRootDescriptorTable(0, gpu);
        cmd->SetComputeRoot32BitConstants(1, sizeof(Params) / 4, &constants, 0);
        cmd->Dispatch((constants.width + 7) / 8, (constants.height + 7) / 8, 1);
    }

    void destroy()
    {
        if (pipeline_ != nullptr)
        {
            pipeline_->Release();
            pipeline_ = nullptr;
        }

        if (root_ != nullptr)
        {
            root_->Release();
            root_ = nullptr;
        }

        if (heap_ != nullptr)
        {
            heap_->Release();
            heap_ = nullptr;
        }

        device_ = nullptr;
    }

  private:
    static const unsigned int kRingSlots = 8;
    static const unsigned int kPerDispatch = 7;

    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pipeline_ = nullptr;
    ID3D12DescriptorHeap* heap_ = nullptr;
    unsigned int stride_ = 0;
    unsigned int ring_ = 0;
};
} // namespace codec
