#pragma once

#include "DlssNr_Switch.h"

#if OPTI_DLSSNR

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

/*
 * Jitter arithmetic for the multi-pass chain, and the checks that say when it is wrong.
 *
 * Three things live here, and all three exist because the multi-pass arrangement introduces a pass
 * whose render target is not the one the game rasterised at. Everything the DLSS Programming Guide
 * says about jitter is stated relative to "the render target size", and a chain with two features at
 * two sizes has two answers to that.
 *
 *   Rescale        Section 3.7.3(2): offsets are in pixel space at the render target size, and the
 *                  guide underlines that they *cannot* be provided at output resolution the way
 *                  motion vectors optionally can. So a pass held below the game's render resolution
 *                  needs the game's offsets scaled by the same ratio its buffers were.
 *
 *   InBounds       Section 3.7.3(1): always between -0.5 and +0.5, since jitter should always result
 *                  in movement within a source pixel. A value outside that is not a jitter value; it
 *                  is a scaling mistake, and section 3.7.4 lists what it looks like -- screen shaking,
 *                  distant objects not resolving, a screen-door pattern, static thin features and fine
 *                  texture detail going fuzzy.
 *
 *   PhaseCounter   Section 3.7.1.1: the pattern has to cycle through enough unique offsets to cover
 *                  the pixel area. The count is not a constant -- it scales with the area ratio -- and
 *                  the RR guide raises the floor to 32 on top of that. Neither number is checkable
 *                  without actually watching the sequence, which is what this does.
 *
 * Header-only and free of D3D, so the D3D12 module, the Vulkan module and the upscaler pipeline can
 * all reach the same arithmetic rather than each doing its own.
 */
namespace DlssNr
{
namespace jitter
{

// Section 3.7.3(1). Offsets outside this are a bug in whoever computed them.
constexpr float kBound = 0.5f;

/*
 * Offsets moved into the pixel space of a pass that rasterised at a different size.
 *
 * Multi-pass Custom holds the first pass below the game's render resolution and resamples its colour,
 * depth and motion vectors down to match. The jitter has to come with them: the game's offsets are in
 * the game's render pixels, and handing those over unscaled overstates the offset by exactly the
 * inverse of the reduction -- a 0.5-pixel offset at 1920 wide describes a 0.33-pixel offset at 1268.
 */
inline float Rescale(float offset, unsigned int srcRes, unsigned int dstRes)
{
    if (srcRes == 0 || dstRes == 0 || srcRes == dstRes || !std::isfinite(offset))
        return offset;

    return offset * ((float) dstRes / (float) srcRes);
}

inline bool InBounds(float x, float y)
{
    return std::isfinite(x) && std::isfinite(y) && std::abs(x) <= kBound && std::abs(y) <= kBound;
}

inline float Clamp(float v) { return std::isfinite(v) ? std::clamp(v, -kBound, kBound) : 0.0f; }

/*
 * Where the resolved pair has to be sampled so its ratio lands on the content the jittered frame
 * actually holds at this pixel.
 *
 * Section 3.7.3(4) says jitter offsets use the same coordinate and direction system as motion
 * vectors, with (0, 0) meaning no jitter. That makes the direction derivable rather than guessed:
 * the offset displaces the rasterised scene, so the content sitting at pixel p in the jittered frame
 * sits at p - j in the frame the first pass resolved. Sampling at -j is therefore the answer, not a
 * coin flip between three values.
 *
 * The one genuinely engine-dependent part is which way each axis points, and the motion vector scale
 * is the best evidence available for that -- it is the value OptiScaler already applies to bring the
 * game's vectors into the convention DLSS wants, so a negative one says that axis is reported the
 * other way round. By the guide's own sentence the jitter is reported in that same system, so it
 * needs the same flip.
 *
 * This is a derivation, not a measurement. It replaces a three-way guess with a one-way one, which is
 * worth having, but the manual override stays because an engine that disagrees with the guide will
 * disagree with this too.
 */
inline void AlignFromJitter(float jitterX, float jitterY, float mvScaleX, float mvScaleY, float& outX,
                            float& outY)
{
    const float sx = (std::isfinite(mvScaleX) && mvScaleX < 0.0f) ? -1.0f : 1.0f;
    const float sy = (std::isfinite(mvScaleY) && mvScaleY < 0.0f) ? -1.0f : 1.0f;

    outX = -jitterX * sx;
    outY = -jitterY * sy;
}

/*
 * How many unique offsets the guide wants behind a pass of this geometry.
 *
 * Section 3.7.1.1 gives it as Total Phases = Base * (Target / Render)^2 with a base of 8, and works
 * the 1080p->4K case out to 32. The square is there because it is the pixel *area* ratio that decides
 * how many samples each output pixel needs to see, so the two axes are multiplied here rather than
 * one being assumed to stand for both -- which matters the moment a game scales anisotropically.
 *
 * The guide's own table falls out of it exactly: DLAA 8, Quality 18, Balanced 24, Performance 32,
 * Ultra Performance 72.
 */
inline unsigned int RecommendedPhases(unsigned int renderW, unsigned int renderH, unsigned int outW,
                                      unsigned int outH)
{
    if (renderW == 0 || renderH == 0 || outW == 0 || outH == 0)
        return 8;

    const double area = ((double) outW / (double) renderW) * ((double) outH / (double) renderH);
    const long phases = std::lround(8.0 * std::max(1.0, area));
    return (unsigned int) std::clamp<long>(phases, 8, 4096);
}

/*
 * The floor for a Ray Reconstruction pass.
 *
 * The RR guide's section 3.6 adds to the SR rule rather than replacing it: there is no reason to
 * limit the sample count, and at least 32 positions is highly recommended. So an RR pass at 1:1,
 * where the SR formula asks for only 8, still wants 32.
 */
inline unsigned int RecommendedPhasesRr(unsigned int renderW, unsigned int renderH, unsigned int outW,
                                        unsigned int outH)
{
    return std::max(32u, RecommendedPhases(renderW, renderH, outW, outH));
}

/*
 * Watches the offsets actually going past and counts how many distinct ones there were.
 *
 * A phase count cannot be read out of any parameter -- the game never states it -- so the only way to
 * know whether a title meets the guide's recommendation is to observe the sequence until it repeats.
 * That is also the only way to catch the failure this is really here for: a chain that hands a pass
 * one fixed offset every frame reports exactly one phase, and reads on screen as a frame that never
 * resolves.
 *
 * Quantised, because a Halton sequence returns the same phase as the same float and anything else is
 * a different phase. The tolerance is far below the smallest step a 4096-phase pattern would take.
 */
class PhaseCounter
{
  public:
    void observe(float x, float y)
    {
        if (!std::isfinite(x) || !std::isfinite(y))
            return;

        ++seen_;
        minX_ = std::min(minX_, x);
        maxX_ = std::max(maxX_, x);
        minY_ = std::min(minY_, y);
        maxY_ = std::max(maxY_, y);

        if (!InBounds(x, y))
            ++outOfBounds_;

        const uint32_t key = quantise(x, y);

        for (unsigned int i = 0; i < count_; ++i)
        {
            if (keys_[i] == key)
            {
                ++repeats_;
                return;
            }
        }

        repeats_ = 0;

        if (count_ < kMax)
            keys_[count_++] = key;
        else
            ++overflow_;
    }

    // Distinct offsets observed. Saturates at the table size plus whatever fell off it, which is only
    // reached by a pattern already far past any recommendation.
    unsigned int distinct() const { return count_ + overflow_; }

    unsigned int observations() const { return seen_; }

    // The sequence has come round: every offset for a while now has been one already recorded, so the
    // count is the whole pattern rather than a prefix of it.
    bool settled() const { return repeats_ >= kSettle; }

    unsigned int outOfBounds() const { return outOfBounds_; }

    void extents(float& lox, float& hix, float& loy, float& hiy) const
    {
        lox = minX_;
        hix = maxX_;
        loy = minY_;
        hiy = maxY_;
    }

    void reset() { *this = PhaseCounter(); }

  private:
    static constexpr unsigned int kMax = 256;

    // Consecutive repeats before the pattern is called complete. A Halton sequence of N phases gives
    // N-1 of these in a row once it cycles, so this settles quickly on anything real while still
    // outlasting a chance collision early in the sequence.
    static constexpr unsigned int kSettle = 8;

    static uint32_t quantise(float x, float y)
    {
        const int32_t qx = (int32_t) std::lround(std::clamp(x, -8.0f, 8.0f) * 8192.0f);
        const int32_t qy = (int32_t) std::lround(std::clamp(y, -8.0f, 8.0f) * 8192.0f);
        return ((uint32_t) (qx & 0xFFFF) << 16) | (uint32_t) (qy & 0xFFFF);
    }

    std::array<uint32_t, kMax> keys_ {};
    unsigned int count_ = 0;
    unsigned int overflow_ = 0;
    unsigned int repeats_ = 0;
    unsigned int seen_ = 0;

    unsigned int outOfBounds_ = 0;

    float minX_ = 0.0f;
    float maxX_ = 0.0f;
    float minY_ = 0.0f;
    float maxY_ = 0.0f;
};

} // namespace jitter
} // namespace DlssNr

#endif
