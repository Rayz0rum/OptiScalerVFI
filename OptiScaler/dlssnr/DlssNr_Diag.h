#pragma once

#include "DlssNr_Switch.h"

#if OPTI_DLSSNR

#include <d3d12.h>
#include <gpu_time/GpuTime_Dx12.h>

#include <array>
#include <memory>
#include <optional>

/*
 * Where the frame's time actually goes, per pass.
 *
 * The module has measured one number until now -- the whole Neural Rendering pass -- which is enough
 * to say the feature is expensive and nothing at all about why. Neural Rendering, Super Resolution
 * and frame generation all want the same tensor units, so on a power-limited part a share of the
 * measured cost is contention rather than work, and no amount of reasoning distinguishes the two from
 * a single total.
 *
 * That distinction decides what is worth building. Running the model at reduced resolution, or every
 * other frame, only pays if inference is the cost; if the passes are serialising against each other,
 * or the encode and resolve dispatches are a bigger share than assumed, both are effort spent on the
 * wrong thing. So this is deliberately the first piece of work, and the optimisation it is meant to
 * guide comes after it rather than beside it.
 *
 * Each stage gets its own timer because they are recorded at different points on the list and
 * sometimes on different lists. Timestamp pairs nest badly; separate heaps do not.
 */
namespace DlssNr
{
namespace diag
{

enum class Stage : unsigned int
{
    // The colour codec's first dispatch: linear HDR to the display-referred picture the model reads.
    Encode = 0,

    // The optional reduction ahead of the model, when it is working below full resolution.
    Downsample,

    // The model itself. The number every performance question is really about.
    Inference,

    // The codec's second dispatch, folding the answer back onto the untouched frame.
    Resolve,

    // Measuring the edit on the resolved frame and carrying it onto the jittered one, in multi-pass.
    Transfer,

    // The second Super Resolution feature performing the enlargement, in multi-pass.
    Enlarge,

    Count
};

inline const char* StageName(Stage s)
{
    switch (s)
    {
    case Stage::Encode:
        return "encode";
    case Stage::Downsample:
        return "downsample";
    case Stage::Inference:
        return "inference";
    case Stage::Resolve:
        return "resolve";
    case Stage::Transfer:
        return "transfer";
    case Stage::Enlarge:
        return "enlarge";
    default:
        return "?";
    }
}

/*
 * One timer per stage, built on the first use and read once a frame.
 *
 * Reads are deliberately lazy and tolerant of returning nothing: a timestamp pair is only available a
 * few frames after it was written, and a stage that did not run this frame simply keeps its last
 * value rather than reporting zero -- zero would read as "free" in the overlay, which is the one
 * answer that is never true.
 */
class StageTimers
{
  public:
    void ensure(ID3D12Device* device)
    {
        if (device == nullptr)
            return;

        for (auto& t : timers_)
        {
            if (t == nullptr)
                t = std::make_unique<GpuTime_Dx12>(device);
        }
    }

    void start(Stage s, ID3D12GraphicsCommandList* cmdList)
    {
        auto& t = timers_[(unsigned int) s];

        if (t != nullptr && cmdList != nullptr)
        {
            t->Start(cmdList);
            ran_[(unsigned int) s] = true;
        }
    }

    void end(Stage s, ID3D12GraphicsCommandList* cmdList)
    {
        auto& t = timers_[(unsigned int) s];

        if (t != nullptr && cmdList != nullptr)
            t->End(cmdList);
    }

    // Called once a frame with the queue the work went to. Stages that have no result yet keep the
    // value they had.
    void read(ID3D12CommandQueue* queue)
    {
        if (queue == nullptr)
            return;

        for (unsigned int i = 0; i < (unsigned int) Stage::Count; ++i)
        {
            if (timers_[i] == nullptr)
                continue;

            if (auto ms = timers_[i]->ReadGpuTime(queue); ms.has_value())
                last_[i] = ms;
        }
    }

    std::optional<double> get(Stage s) const { return last_[(unsigned int) s]; }

    // Whether the stage was recorded at all since the last clear. Distinguishes "costs nothing" from
    // "did not happen", which the overlay has to show differently.
    bool ran(Stage s) const { return ran_[(unsigned int) s]; }

    // The stages that did run, added up. Not the frame's NR cost -- passes overlap on the GPU -- but
    // the right number for asking what share a given stage is of the work.
    double total() const
    {
        double sum = 0.0;

        for (unsigned int i = 0; i < (unsigned int) Stage::Count; ++i)
        {
            if (ran_[i] && last_[i].has_value())
                sum += *last_[i];
        }

        return sum;
    }

    // Start of frame: forget which stages ran, keep what they cost.
    void beginFrame() { ran_ = {}; }

    void destroy()
    {
        for (auto& t : timers_)
            t.reset();

        last_ = {};
        ran_ = {};
    }

  private:
    std::array<std::unique_ptr<GpuTime_Dx12>, (size_t) Stage::Count> timers_ {};
    std::array<std::optional<double>, (size_t) Stage::Count> last_ {};
    std::array<bool, (size_t) Stage::Count> ran_ {};
};

// Records a stage across a scope, so an early return cannot leave a timestamp pair half-written.
class ScopedStage
{
  public:
    ScopedStage(StageTimers& timers, Stage stage, ID3D12GraphicsCommandList* cmdList)
        : timers_(timers), stage_(stage), cmdList_(cmdList)
    {
        timers_.start(stage_, cmdList_);
    }

    ~ScopedStage() { timers_.end(stage_, cmdList_); }

    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;

  private:
    StageTimers& timers_;
    Stage stage_;
    ID3D12GraphicsCommandList* cmdList_;
};

} // namespace diag
} // namespace DlssNr

#endif
