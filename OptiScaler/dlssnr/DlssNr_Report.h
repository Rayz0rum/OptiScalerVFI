#pragma once

#include "DlssNr_Switch.h"

#if OPTI_DLSSNR

#include <string>

/*
 * One line in the log that says what the frame was actually built out of.
 *
 * Every bug report about this feature so far has cost a round trip to establish facts the code
 * already knew at the time: which arrangement ran, what size each pass was, whether the game's motion
 * vectors were render or display resolution, whether the first pass was Ray Reconstruction, whether
 * the second one got jitter. None of that is guessable from a screenshot, and asking for it one
 * question at a time is how a five-minute diagnosis becomes a week.
 *
 * So it is emitted once, as a single greppable line, the first time a configuration evaluates -- and
 * again whenever any of it changes, because a placement change mid-session is exactly the case where
 * a stale line would mislead.
 *
 * No graphics API in here on purpose: the D3D12 path, the Vulkan path and the upscaler pipeline all
 * fill in the same structure, so a report from one backend reads like a report from another.
 */
namespace DlssNr
{
namespace report
{

// A number that is genuinely not applicable, rather than zero. Preset especially: 0 is "default",
// which is a real preset and a different statement from "this pass has no preset".
constexpr int kNotApplicable = -1;

struct Pass
{
    // Short and fixed, so the line can be grepped for: "RR1", "SR1", "NR", "SR2", "SPATIAL".
    const char* name = "";

    unsigned int inW = 0;
    unsigned int inH = 0;
    unsigned int outW = 0;
    unsigned int outH = 0;

    int preset = kNotApplicable;

    // What the feature was CREATED with, not what the frame happens to be. The two disagree on
    // purpose in the reordered arrangements, and that disagreement is the thing worth logging.
    bool hdr = false;

    // Whether this pass was handed an exposure texture at all. The RR guide is explicit that Ray
    // Reconstruction does not support exposure, so an RR pass showing exposure=1 is a bug and this is
    // where it becomes visible.
    bool exposure = false;

    // Whether the pass received the game's real jitter offsets or zeros. Only ever false for a final
    // enlargement pass reading an already-resolved image.
    bool jitter = true;

    // Distinct offsets observed so far, and how many the guide asks for at this geometry. phases
    // below wanted is section 3.7.1.1's failure, and it looks like a frame that will not resolve.
    unsigned int phases = 0;
    unsigned int phasesWanted = 0;

    // Set once the sequence has been seen to repeat, so a low count can be trusted as final rather
    // than merely early.
    bool phasesSettled = false;
};

struct Integration
{
    const char* backend = "?";

    // The whole chain in one token: "post", "nr>sr", "sr1>nr>sr2", "rr1>nr>sr2", "sr1>nr>spatial".
    const char* topology = "?";

    // What the GAME declared, which is what every downstream decision is supposed to follow.
    bool mvLowRes = true;
    bool mvJittered = false;
    bool depthInverted = false;
    bool gameHdr = false;

    // Render to display, on the long axis. The single number that predicts most of the quality.
    float scaleFactor = 1.0f;

    static constexpr unsigned int kMaxPasses = 4;
    Pass passes[kMaxPasses] {};
    unsigned int passCount = 0;

    void add(const Pass& p)
    {
        if (passCount < kMaxPasses)
            passes[passCount++] = p;
    }
};

namespace detail
{
inline void appendUInt(std::string& s, unsigned int v) { s += std::to_string(v); }

inline void appendFloat(std::string& s, float v)
{
    char buf[32] = {};
    // Two decimals: enough to tell 1.50 from 1.72, short enough to stay on one line.
    snprintf(buf, sizeof(buf), "%.2f", v);
    s += buf;
}

inline void appendBool(std::string& s, bool v) { s += v ? "1" : "0"; }
} // namespace detail

/*
 * The line itself. Deliberately dense and machine-shaped rather than prose: it is going to be read
 * out of somebody else's log file by someone searching for one token.
 *
 * `withPhases` is what separates the line from the key it is latched on. Phase counts climb for the
 * first few dozen frames of every run, so including them in the comparison would re-emit the line on
 * almost every frame until the sequence cycles. The structure is what is worth watching for changes;
 * the counts are worth printing.
 */
inline std::string Format(const Integration& in, bool withPhases = true)
{
    std::string s = "DLSS-NR integration: backend=";
    s += in.backend;
    s += " topology=";
    s += in.topology;
    s += " scale=";
    detail::appendFloat(s, in.scaleFactor);
    s += " game[hdr=";
    detail::appendBool(s, in.gameHdr);
    s += " mvLowRes=";
    detail::appendBool(s, in.mvLowRes);
    s += " mvJittered=";
    detail::appendBool(s, in.mvJittered);
    s += " depthInverted=";
    detail::appendBool(s, in.depthInverted);
    s += "]";

    for (unsigned int i = 0; i < in.passCount; ++i)
    {
        const Pass& p = in.passes[i];

        s += " ";
        s += p.name;
        s += "[";
        detail::appendUInt(s, p.inW);
        s += "x";
        detail::appendUInt(s, p.inH);
        s += "->";
        detail::appendUInt(s, p.outW);
        s += "x";
        detail::appendUInt(s, p.outH);

        if (p.preset != kNotApplicable)
        {
            s += " preset=";
            s += std::to_string(p.preset);
        }

        s += " hdr=";
        detail::appendBool(s, p.hdr);
        s += " exposure=";
        detail::appendBool(s, p.exposure);
        s += " jitter=";
        detail::appendBool(s, p.jitter);

        if (withPhases && p.phasesWanted != 0)
        {
            s += " phases=";
            detail::appendUInt(s, p.phases);
            s += "/";
            detail::appendUInt(s, p.phasesWanted);

            // An unsettled count is a prefix of the pattern, not the pattern. Saying so stops a
            // reader concluding a title is short on phases when it has simply not cycled yet.
            if (!p.phasesSettled)
                s += "?";
        }

        s += "]";
    }

    return s;
}

/*
 * Emits the line when it first exists and whenever it stops being true.
 *
 * Comparing the formatted string rather than the fields is deliberate: it cannot drift out of sync
 * with what Format actually prints, and the cost is one string compare on a path that already
 * touches the GPU.
 */
class Latch
{
  public:
    // Returns the line to log, or an empty string when nothing has changed since last time.
    std::string update(const Integration& in)
    {
        std::string key = Format(in, false);

        if (key == last_)
            return std::string();

        last_ = key;
        return Format(in, true);
    }

    void reset() { last_.clear(); }

  private:
    std::string last_;
};

} // namespace report
} // namespace DlssNr

#endif
