#include "pch.h"

#include "DlssNr.h"

#if OPTI_DLSSNR

#include <Config.h>
#include <menu/menu_common.h>

#include <imgui/imgui.h>

namespace
{
// A process runs one backend, never several -- a D3D11 game never reaches the D3D12 path, and a
// Vulkan one reaches neither -- so the panel asks all three and reports whichever answered. Without
// this the D3D11 and Vulkan routes work while the menu insists nothing is running, which reads as a
// bug in the pass rather than in the menu.
bool AnyRunning() { return DlssNr::IsRunning() || DlssNr::IsRunningDx11() || DlssNr::IsRunningVk(); }

const char* AnyFailureReason()
{
    const char* reason = DlssNr::FailureReason();
    if (reason[0] != 0)
        return reason;

    reason = DlssNr::FailureReasonDx11();
    return reason[0] != 0 ? reason : DlssNr::FailureReasonVk();
}

std::optional<double> AnyGpuTime()
{
    const auto ms = DlssNr::LastGpuTime();
    return ms.has_value() ? ms : DlssNr::LastGpuTimeDx11();
}

void RetryAll()
{
    DlssNr::RetryAfterFailure();
    DlssNr::RetryAfterFailureDx11();
    DlssNr::RetryAfterFailureVk();
}
} // namespace

namespace DlssNr
{

// The "(?)" marker every control carries, matching the rest of the menu.
static void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void RenderMenu(Config* config, float menuResScale)
{

    // DLSS Neural Rendering -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("DLSS Neural Rendering"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool enabled = config->DlssNrEnabled.value_or_default();
        if (ImGui::Checkbox("Enable Neural Rendering", &enabled))
            config->DlssNrEnabled = enabled;

        HelpMarker("Synthesises detail in the upscaler's output, before frame generation sees it."
                       "\n\nNeeds two similarly named files beside OptiScaler, one character apart:"
                       "\n  nvngx_dlssnr.dll       NVIDIA's model (~165 MB) -- you supply it"
                       "\n  nvngx.dll_dlssnr.dll   the forwarder (~13 KB) -- ships in this package"
                       "\nUndocumented and driven directly, so none of this is officially supported.");

        if (!AnyRunning())
        {
            const char* reason = AnyFailureReason();

            if (reason[0] != 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Off for this session: %s.", reason);
                ImGui::SameLine();

                if (ImGui::SmallButton("Retry"))
                    RetryAll();
            }
            else if (enabled)
                ImGui::TextUnformatted("Waiting for the upscaler to run.");
        }
        else
        {
            // The cost belongs here rather than only in the upscaler's breakdown: that tooltip needs
            // OptiScaler's own upscaler to have run, and with native DLSS passing through there is
            // nothing in it to hang this off.
            const auto ms = AnyGpuTime();

            if (ms.has_value())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running - %.2f ms per frame",
                                   ms.value());
            else
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running.");

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("The whole pass: the staging copies and the resolve as well as the"
                                  "\nmodel. Timing only the model would flatter the number."
                                  "\n\nCompare it against the frame time at the bottom of this window to"
                                  "\nsee what it is costing you.");

            /*
             * The split, not just the total.
             *
             * A single number says the feature is expensive and nothing about why, and the two
             * answers point at completely different work: if inference dominates, running the model
             * smaller or less often pays; if the codec's own dispatches or the enlargement do, both
             * are effort spent in the wrong place. Neural Rendering, Super Resolution and frame
             * generation also compete for the same tensor units, so on a power-limited part a share
             * of this is contention rather than work -- which only shows up as a stage costing more
             * here than the same stage costs on its own.
             */
            if (ImGui::TreeNode("Cost by stage"))
            {
                const DlssNr::diag::Stage stages[] = {
                    DlssNr::diag::Stage::Encode,    DlssNr::diag::Stage::Downsample,
                    DlssNr::diag::Stage::Inference, DlssNr::diag::Stage::Resolve,
                    DlssNr::diag::Stage::Transfer,  DlssNr::diag::Stage::Enlarge
                };

                const double total = DlssNr::StageTotal();

                for (const auto stage : stages)
                {
                    const auto value = DlssNr::StageTime(stage);

                    if (!value.has_value())
                    {
                        // A dash rather than 0.00: a stage that did not run in this arrangement has
                        // no cost to report, and a zero would read as "free".
                        ImGui::TextDisabled("%-11s      -", DlssNr::diag::StageName(stage));
                        continue;
                    }

                    ImGui::Text("%-11s %6.3f ms  %4.1f%%", DlssNr::diag::StageName(stage), *value,
                                total > 0.0 ? (*value / total) * 100.0 : 0.0);
                }

                ImGui::Separator();
                ImGui::Text("%-11s %6.3f ms", "measured", total);

                HelpMarker("Timestamp pairs around each dispatch, read back a few frames later."
                           "\n\nThe stages overlap on the GPU, so the sum is not the frame's real"
                           "\ncost -- it is the right denominator for asking what share each stage"
                           "\nis, which is the question that decides what is worth optimising."
                           "\n\nA dash means the stage did not run in this arrangement.");

                ImGui::TreePop();
            }
        }

        ImGui::Spacing();
        ImGui::PushItemWidth(220.0f * menuResScale);

        ImGui::SeparatorText("Placement");

        {
            static const char* modeNames[] = { "Post-process (Recommended)", "Upscale with DLSS-SR",
                                               "Multi-pass Rendering",
                                               "Multi-pass Rendering (Custom)" };

            int mode = (int) config->DlssNrMode.value_or_default();

            if (ImGui::Combo("Placement", &mode, modeNames, IM_ARRAYSIZE(modeNames)))
                config->DlssNrMode = (uint32_t) mode;

            HelpMarker("Where the model sits relative to the upscaler."
                           "\n\nPost-process runs it on the finished frame. It is the only one that needs"
                           "\nnothing from the upscaler, so it is also the only one that works when a game"
                           "\nis using its own DLSS and OptiScaler is passing it straight through."
                           "\n\nUpscale with DLSS-SR runs the model at render resolution and lets Super"
                           "\nResolution enlarge its result, so the upscaler's temporal accumulation works"
                           "\non enhanced pixels instead of the model re-deciding detail on every enlarged"
                           "\nframe. The upscaler's feature is created with IsHDR and AutoExposure cleared"
                           "\nto match, so changing to or from this rebuilds it."
                           "\n\nMulti-pass runs a first pass 1:1 -- denoising if that is Ray Reconstruction,"
                           "\nantialiasing as DLAA if it is Super Resolution -- then the model, then a"
                           "\nsecond Super Resolution feature does the single enlargement. It costs a"
                           "\nsecond set of temporal history and the memory for it."
                           "\n\nAnything but post-process applies to OptiScaler's own upscalers only.");

            const auto selected = (DlssNr::Mode) config->DlssNrMode.value_or_default();

            /*
             * The one placement that hands the model an aliased picture, said out loud.
             *
             * The model reads high-frequency texture detail to decide what material a surface is, so
             * its input wants to be as detailed as possible AND clean. Those pull in opposite
             * directions and only one arrangement satisfies both: a 1:1 first pass, which resolves
             * the game's jitter into antialiased detail at render resolution before the model sees
             * it. This one skips that and shows the model the game's raw jittered buffer, where the
             * high frequencies present are aliasing rather than material -- so what comes back is not
             * a weaker version of the same answer, it is a different one.
             */
            if (selected == DlssNr::Mode::UpscaleWithSR)
            {
                ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f),
                                   "The model is being shown an aliased picture.");
                HelpMarker("This placement runs the model on the game's raw render-resolution"
                               "\nbuffer, before anything has resolved it -- so the high-frequency"
                               "\ndetail it sees is aliasing rather than material."
                               "\n\nThat matters more than it sounds. The model reads fine texture"
                               "\ndetail to work out what surface it is looking at, so feeding it"
                               "\naliasing does not weaken the answer, it changes it."
                               "\n\nMulti-pass Rendering is the arrangement that gets this right: its"
                               "\nfirst pass resolves the jitter into clean antialiased detail at"
                               "\nrender resolution, which is both the most detail available and the"
                               "\ncleanest form of it.");
            }

            if (DlssNr::UsesTwoFeatures(selected))
            {
                static const char* pipelineNames[] = { "Ray Reconstruction", "Super Resolution" };
                int pipeline = (int) config->DlssNrFeature1Pipeline.value_or_default();

                if (ImGui::Combo("First pass", &pipeline, pipelineNames, IM_ARRAYSIZE(pipelineNames)))
                    config->DlssNrFeature1Pipeline = (uint32_t) pipeline;

                HelpMarker("Which upscaler the first pass is."
                               "\n\nThis states what the game is set up for; it does not switch anything."
                               "\nOptiScaler cannot substitute one for the other -- Ray Reconstruction needs"
                               "\nG-buffer inputs a Super Resolution integration never supplies -- so a"
                               "\nmismatch falls back to post-process rather than half-applying the"
                               "\narrangement, and says so in the log.");

                {
                    /*
                     * The alignment used to be a three-way guess. It is now derived, and the guess is
                     * kept only as an override -- so the default is first and the manual signs read
                     * as what they are.
                     */
                    static const char* alignNames[] = { "Derived from the motion vectors (Recommended)",
                                                        "Manual: +1", "Manual: -1", "Off" };
                    const int alignValues[] = { 2, 1, -1, 0 };

                    const int current = config->DlssNrMultiPassAlign.value_or_default();
                    int index = 0;

                    for (int i = 0; i < IM_ARRAYSIZE(alignValues); ++i)
                    {
                        if (alignValues[i] == current)
                            index = i;
                    }

                    if (ImGui::Combo("Edit alignment", &index, alignNames, IM_ARRAYSIZE(alignNames)))
                        config->DlssNrMultiPassAlign = alignValues[index];

                    HelpMarker("Where the transfer samples the resolved frame to line its edit up with"
                               "\nthe jittered one."
                               "\n\nThe ratio is measured on the resolved frame and applied to the"
                               "\njittered one, and the same scene feature sits up to half a pixel apart"
                               "\nin the two. Get the direction wrong and the misalignment doubles rather"
                               "\nthan cancelling -- and the enlargement then averages it away across"
                               "\njitter offsets, taking the model's work with it. That is what \"the"
                               "\ntransfer makes NR do almost nothing\" looks like."
                               "\n\nDerived is the default. The programming guide states jitter offsets"
                               "\nuse the same coordinate and direction system as motion vectors, so the"
                               "\ndirection follows from the motion vector convention the game already"
                               "\ndeclares rather than from trying values."
                               "\n\nThe manual signs remain for an engine that disagrees with the guide."
                               "\nOff disables the alignment entirely.");

                    float history = config->DlssNrRatioHistory.value_or_default();

                    if (ImGui::SliderFloat("Ratio history", &history, 0.0f, 0.95f, "%.2f"))
                        config->DlssNrRatioHistory = history;

                    HelpMarker("The ghosting control."
                               "\n\nThe enlargement is a temporal reconstructor: it accumulates"
                               "\nsamples of what it believes is one surface and resolves"
                               "\ndisagreement between them by smearing. A ratio that changes frame"
                               "\nto frame on a static surface is exactly that disagreement -- the"
                               "\nmodel re-decides part of its answer each frame, and the alignment"
                               "\noffset that positions the ratio moves with the jitter."
                               "\n\nThis averages the ratio field along the surface first, carried"
                               "\nforward by motion vectors, with a neighbourhood clamp throwing the"
                               "\nhistory away across disocclusions and moving objects. What survives"
                               "\nthe average is the model's real decision about the material; what"
                               "\ncancels is the per-frame churn."
                               "\n\nUse this rather than turning Tone strength down. Tone is the"
                               "\nmodel's low-frequency verdict on light and colour and a large part"
                               "\nof what it is for -- lowering it hides the ghosting by removing the"
                               "\neffect."
                               "\n\nHigher is steadier and slower to respond. 0 is the old behaviour.");

                    float damping = config->DlssNrHighlightDamping.value_or_default();

                    if (ImGui::SliderFloat("Highlight damping", &damping, 0.0f, 1.0f, "%.2f"))
                        config->DlssNrHighlightDamping = damping;

                    HelpMarker("How completely the transferred edit fades out above the white point."
                               "\n\nThe model's opinion is least reliable there by construction: the"
                               "\nencode bounds luminance but not individual channels, so a pixel above"
                               "\nwhite was shown to it already clipped. And a multiplicative edit"
                               "\namplifies whatever variation it carries in proportion to what it"
                               "\nmultiplies, so any residual wobble is loudest at the brightest"
                               "\npixels -- which reads as sparkle rather than as noise."
                               "\n\nThis is a trade, not a free fix: it costs the model its highlight"
                               "\nwork. 0 restores the unfaded behaviour exactly. Turn it down if"
                               "\nhighlights look flat; leave it up if they flicker.");

                    bool matchColour = config->DlssNrMatchGameColourSpace.value_or_default();

                    if (ImGui::Checkbox("Match the game's colour space", &matchColour))
                        config->DlssNrMatchGameColourSpace = matchColour;

                    HelpMarker("Whether the enlargement is created for the colour space it is"
                               "\nactually handed."
                               "\n\nIt used to be created with IsHDR and AutoExposure cleared, on the"
                               "\npremise that Neural Rendering hands it a tone-mapped picture. It does"
                               "\nnot: the pass returns the frame in whatever space it received, and"
                               "\nthe edit transfer applies a near-unity ratio to the game's own"
                               "\nbuffer. What arrives is the game's colour space."
                               "\n\nDeclaring that display-referred selects DLSS's LDR path, which"
                               "\nquantises to 8 bits and expects a perceptually linear encoding."
                               "\nGiving it linear colour instead is the programming guide's own"
                               "\naccount of banding and colour shifting -- the most likely reason the"
                               "\nmodel's colours stop matching the upscaler's."
                               "\n\nChanging this rebuilds the enlargement, so expect one reset frame.");
                }

                static const char* enlargeNames[] = { "DLSS Super Resolution (Recommended)", "Spatial (safe fallback)" };
                int enlarge = (int) config->DlssNrMultiPassEnlarge.value_or_default();

                if (ImGui::Combo("Enlargement", &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames)))
                    config->DlssNrMultiPassEnlarge = (uint32_t) enlarge;

                HelpMarker("How the chain gets from the first pass's resolution to the display."
                               "\n\nThe first pass resolves the game's jitter -- that is what DLAA and Ray"
                               "\nReconstruction are for -- so what reaches the enlargement is"
                               "\ngrid-aligned, with no subpixel variation left."
                               "\n\nA second DLSS pass then reconstructs from one sample position per"
                               "\npixel, identical every frame, while the model re-decides detail"
                               "\nunderneath it. That is soft, and it warps whenever the camera moves."
                               "\nChaining two temporal passes cannot preserve jitter for the second"
                               "\none: it is a property of the arrangement, not a fault in it."
                               "\n\nThe spatial filter asks for no jitter and keeps no history, so"
                               "\nneither failure is available to it. It is no sharper -- both are"
                               "\nlimited to what the first pass produced -- but it is steady."
                               "\n\nIf you want DLSS doing the enlargement, use Upscale with DLSS-SR"
                               "\ninstead: one temporal pass, with the game's jitter intact.");
            }

            if (selected == DlssNr::Mode::MultiPassCustom)
            {
                /*
                 * The slider edits a pending value and only the button commits it.
                 *
                 * Everything this changes is latched when a feature is created, and it
                 * changes two of them: the first pass and the second upscaler both have
                 * to be rebuilt. Acting on each intermediate value as the slider moved
                 * would rebuild both dozens of times in a drag and exhaust the driver's
                 * create-time latches, after which the model stops responding until the
                 * process restarts.
                 */
                static int pending = -1;
                const int applied = config->DlssNrFeature1Scale.value_or_default();

                if (pending < 0)
                    pending = applied;

                ImGui::SliderInt("First pass scale %", &pending, 0, 100);

                HelpMarker("The first pass's height as a percentage of the display height."
                               "\n\n0 leaves it at the game's own render resolution, which is the plain"
                               "\nmulti-pass arrangement. Lower buys back what the second feature costs."
                               "\n\nThe floor is Ultra Performance -- a third of the display height. Below"
                               "\nthat the first pass has less to work with than any shipping DLSS preset"
                               "\nwould hand it, and nothing downstream can invent what was discarded, so"
                               "\nthe value is clamped rather than obeyed."
                               "\n\nNothing happens until you press Apply: this rebuilds both features, and"
                               "\ndoing that on every value the slider passes through would burn out the"
                               "\ndriver's create-time latches.");

                ImGui::SameLine();

                const bool dirty = pending != applied;

                ImGui::BeginDisabled(!dirty);

                if (ImGui::Button("Apply Scale"))
                    config->DlssNrFeature1Scale = pending;

                ImGui::EndDisabled();

                if (dirty)
                {
                    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "Applied: %d%%. Press Apply to change.",
                                       applied);
                }
            }
        }

        ImGui::SeparatorText("Cost");

        static const char* scaleNames[] = { "Full resolution", "75%", "50%", "33%" };
        static const float scaleValues[] = { 1.0f, 0.75f, 0.5f, 0.3333f };
        const float currentScale = config->DlssNrWorkingScale.value_or_default();
        int scaleIndex = 0;
        for (int i = 0; i < IM_ARRAYSIZE(scaleValues); ++i)
        {
            if (currentScale <= scaleValues[i] + 0.01f)
                scaleIndex = i;
        }
        if (ImGui::Combo("Model resolution", &scaleIndex, scaleNames, IM_ARRAYSIZE(scaleNames)))
            config->DlssNrWorkingScale = scaleValues[scaleIndex];

        HelpMarker("What fraction of the frame the model works at. Cost falls with the square of"
                       "\nthis, so half resolution is roughly a quarter of the time."
                       "\n\nThe frame is never reduced. Only the model's contribution is computed small"
                       "\nand enlarged, so the picture underneath is untouched whatever this says."
                       "\n\nWhat it trades: the shading the model adds is broad and survives enlargement;"
                       "\nthe fine structure it synthesises does not, and softens. Worth having when the"
                       "\npass costs more than you want to pay for the detail it returns."
                       "\n\nThe frame itself stays at full detail whatever this says -- only the"
                       "\nmodel's own work is done small."
                       "\n\nThere is a reason to be careful with it beyond softness. The model reads"
                       "\nhigh-frequency texture detail to work out what MATERIAL it is looking at,"
                       "\nand shrinking the picture first is a low-pass -- it removes exactly the"
                       "\nevidence that decision rests on. So this does not simply scale the effect"
                       "\ndown; past a point it changes what the model thinks the surface is.");

    if (config->DlssNrWorkingScale.value_or_default() < 0.99f)
    {
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f),
                           "The model is being shown a low-passed picture.");
        HelpMarker("Below full resolution the model is shown a shrunk copy, and the fine texture"
                       "\ndetail it uses to identify materials is the first thing a shrink removes."
                       "\nIf the look changes character rather than just weakening, this is why.");
    }

    ImGui::SeparatorText("How much of it lands");

        /*
         * This used to be called "Detail strength" too, which collided with the band control below
         * -- two widgets with one label are one ImGui ID, and the library says so when you hover
         * either. The names were also simply wrong: this one is the whole edit, and the band
         * controls are halves of it.
         */
        float transfer = config->DlssNrTransferStrength.value_or_default();
        if (ImGui::SliderFloat("Overall strength", &transfer, 0.0f, 2.0f, "%.2f"))
            config->DlssNrTransferStrength = transfer;

        HelpMarker("How far the frame moves toward the model's picture, before Tone and Detail"
                       "\nsplit that movement into halves."
                       "\n\nThe model's answer is not added to the frame -- it is a complete picture of its"
                       "\nown, rescaled so its luminance sits where the original says it should. This"
                       "\nblends between the two, so both ends are real pictures and everything between"
                       "\nthem is one too."
                       "\n\n0 gives back exactly what the upscaler produced. 1 is the model's picture."
                       "\n\nAbove 1 carries on past it in the same direction, which is not something the"
                       "\nmodel asked for -- use it to see what it is doing, then come back down. This"
                       "\nis the control to push if you want more effect: Intensity belongs to the model"
                       "\nand it decides what to do with it.");

        float tone = config->DlssNrToneStrength.value_or_default();
        if (ImGui::SliderFloat("Tone strength", &tone, 0.0f, 1.0f, "%.2f"))
            config->DlssNrToneStrength = tone;

        HelpMarker("How much of the model's LOW-frequency verdict reaches the frame: light"
                       "\ninteraction, colour, how a surface should sit overall."
                       "\n\nSet this to 0 with Detail strength at 1 to keep the synthesised structure"
                       "\nwhile restoring the game's own tone and colour. That is a coherent"
                       "\npreference rather than a compromise, and Detail strength alone could not"
                       "\nexpress it -- it turned both halves down together."
                       "\n\nThe split is exact: at 1 and 1 the two bands recombine into precisely the"
                       "\nedit that went in, so these cost nothing until you move one.");

        float detail = config->DlssNrDetailStrength.value_or_default();
        if (ImGui::SliderFloat("Detail strength", &detail, 0.0f, 2.0f, "%.2f"))
            config->DlssNrDetailStrength = detail;

        HelpMarker("How much of the model's HIGH-frequency work reaches the frame: the material and"
                       "\nshading structure it synthesises."
                       "\n\nAllowed past 1 because exaggerating an edit is the honest way to see"
                       "\nwhether there is one.");

        float band = config->DlssNrDetailBand.value_or_default();
        if (ImGui::SliderFloat("Band radius", &band, 0.5f, 6.0f, "%.2f px"))
            config->DlssNrDetailBand = band;

        HelpMarker("Where tone stops and detail begins, as a radius in pixels."
                       "\n\nOnly matters once Tone and Detail strength differ: at 1 and 1 the bands"
                       "\nrecombine exactly whatever this is."
                       "\n\nSmaller keeps more of the frame in the tone band; larger moves more of it"
                       "\ninto detail.");

        float compensation = config->DlssNrDetailCompensation.value_or_default();
        if (ImGui::SliderFloat("Hold strength across resolution", &compensation, 0.0f, 1.0f, "%.2f"))
            config->DlssNrDetailCompensation = compensation;

        HelpMarker("Corrects for the model reading weaker the more upscaling sits behind it."
                       "\n\nDetail is synthesised at whatever resolution the model ran at, and anything"
                       "\nthat magnifies its work afterwards -- a reduced working scale enlarged back,"
                       "\nor a Super Resolution pass taking render resolution to display -- spreads"
                       "\nthat detail over more pixels and thins it out. Nothing about the model"
                       "\nchanged; only how many pixels its work ended up covering."
                       "\n\n1 scales the detail band by the ratio of those two resolutions so the"
                       "\napparent strength stays put. Tone is left alone: it is low-frequency and"
                       "\nsurvives magnification largely intact, so lifting it too would overshoot."
                       "\n\nA first-order correction, not a calibrated curve. 0 is the old behaviour.");

        float lowRes = config->DlssNrLowResGain.value_or_default();
        if (ImGui::SliderFloat("Low-resolution gain", &lowRes, 0.0f, 1.0f, "%.2f"))
            config->DlssNrLowResGain = lowRes;

        HelpMarker("Lifts the detail band at output resolutions below 4K."
                       "\n\nThe model synthesises at a fixed scale in PIXELS, so a 4K frame receives"
                       "\nroughly four times as many added features as a 1080p one and reads as far"
                       "\nmore transformed. Nothing is going wrong at 1080p -- there is simply less"
                       "\nframe for the model to work on, and the fine texture detail it reads to"
                       "\nidentify materials is scarcer there too."
                       "\n\nThis is a preference, not a correction, and the distinction matters: it"
                       "\namplifies what the model DID produce so the effect is more visible. It"
                       "\ncannot invent the features a larger frame would have had, and it will not"
                       "\nmake 1080p look like 4K."
                       "\n\nInert at and above 4K, so the resolution where the pass is already at its"
                       "\nstrongest is untouched. 0 disables it.");

        float colour = config->DlssNrColourStrength.value_or_default();
        if (ImGui::SliderFloat("Colour strength", &colour, 0.0f, 1.0f, "%.2f"))
            config->DlssNrColourStrength = colour;

        HelpMarker("Whether the model's colour arrives with its light."
                       "\n\n0 keeps the game's own hue exactly -- every pixel is the original colour with"
                       "\nonly its brightness carrying the model's verdict. Game-accurate colour, with"
                       "\nthe detail. 1 brings the model's colour as well, in its own hue, clamped into"
                       "\nAP1 so nothing unreachable is asked for."
                       "\n\nThis cannot shift hue on its own: it interpolates between two finished"
                       "\npictures rather than adding a colour difference to one, which is what used to"
                       "\nlet a warm subject come back green.");

        float guard = config->DlssNrColourGuard.value_or_default();
        if (ImGui::SliderFloat("Highlight colour guard", &guard, 0.0f, 1.0f, "%.2f"))
            config->DlssNrColourGuard = guard;

        HelpMarker("Protects saturated highlights from being washed white."
                       "\n\nThe encode normalises brightness but not the individual channels, so a very"
                       "\nbright saturated pixel keeps a channel above 1.0 in the picture the model is"
                       "\nshown -- outside the range it was trained on. It has no way to express"
                       "\n\"green, brighter than white\", so it returns near-white, and the highlight"
                       "\nbranch then correctly carries that whiteness up to full brightness. A neon"
                       "\nlight arrives white."
                       "\n\nThis is about the game's frame buffer, not your monitor. It acts when the"
                       "\ngame renders in linear HDR -- which many do, tone-mapping to SDR only at the"
                       "\nvery end -- so an SDR display sees this too. A frame the game already reports"
                       "\nas tone-mapped is copied rather than encoded, and never triggers it."
                       "\n\nIt acts only on pixels that were actually out of range, which is a small"
                       "\npart of the frame. Everywhere else the model's colour is used in full --"
                       "\nincluding every deliberate shift of tone and light interaction it decided on,"
                       "\nwhich is a real part of what it does and not something to give up to fix a"
                       "\nhighlight. That is the difference between this and turning Colour strength"
                       "\ndown: that switches the model's colour work off across the whole frame."
                       "\n\n0 is the old behaviour.");

        ImGui::SeparatorText("Model");

        ImGui::TextUnformatted("Read when the model is built, so a change rebuilds it after a moment.");

        static const char* nrPresetNames[] = { "Default", "Preset 1", "Preset 2", "Preset 3" };
        int preset = (int) config->DlssNrPreset.value_or_default();
        if (ImGui::Combo("Model preset", &preset, nrPresetNames, IM_ARRAYSIZE(nrPresetNames)))
            config->DlssNrPreset = (uint32_t) preset;

        HelpMarker("Default leaves the choice to the model."
                       "\n\nNot the same scale as the super resolution or ray reconstruction presets --"
                       "\nthe same number means something different here.");

        static const char* nrStyleNames[] = { "Default (standard)", "Natural", "Cinematic" };
        int style = (int) config->DlssNrStyle.value_or_default();

        if (style > 2)
            style = 2;

        if (ImGui::Combo("Style", &style, nrStyleNames, IM_ARRAYSIZE(nrStyleNames)))
            config->DlssNrStyle = (uint32_t) style;

        HelpMarker("The model's own processing profiles."
                   "\n\nDefault (standard): the strongest. Boosts local contrast and deepens"
                   "\nlighting, and can oversaturate or look stylised -- most of what reads as"
                   "\n'the model changed my game's look' is this profile."
                   "\n\nNatural: the same detail work with a gentler hand. Keeps skin tones and"
                   "\ntonal balance closer to what the game rendered."
                   "\n\nCinematic: tones down the shine and over-processing for a film-like look."
                   "\n\nRead when the model is built, so a change rebuilds it after a moment. The"
                   "\nnames come from community testing; NVIDIA ships no names in the binaries.");

        float intensity = config->DlssNrIntensity.value_or_default();
        if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 2.0f, "%.2f"))
            config->DlssNrIntensity = intensity;

        HelpMarker("The model's own strength control, applied inside it. Distinct from detail"
                       "\nstrength above, which scales the result afterwards.");

        float localStructure = config->DlssNrLocalStructure.value_or_default();
        if (ImGui::SliderFloat("Local structure", &localStructure, 0.0f, 2.0f, "%.2f"))
            config->DlssNrLocalStructure = localStructure;

        float localTone = config->DlssNrLocalTone.value_or_default();
        if (ImGui::SliderFloat("Local tone", &localTone, 0.0f, 2.0f, "%.2f"))
            config->DlssNrLocalTone = localTone;


        float skin = config->DlssNrSkinStructure.value_or_default();
        if (ImGui::SliderFloat("Skin structure", &skin, -1.0f, 2.0f, "%.2f"))
            config->DlssNrSkinStructure = skin;

        HelpMarker("-1 means follow local structure, and is the model's own default -- it is not a"
                       "\nstrength of zero. 0 and above set skin independently of the rest of the frame.");

        bool autoMask = config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox("Auto skin mask", &autoMask))
            config->DlssNrAutoMask = autoMask;

        HelpMarker("Lets the model find skin itself rather than treating the frame uniformly.");

        ImGui::SeparatorText("Colour");

        ImGui::TextDisabled("The model was trained on finished, sRGB-encoded frames. The upscaler's\n"
                            "output is not one: it is linear and open-ended. These decide how it is\n"
                            "mapped into something the model recognises. A frame the game reports as\n"
                            "already tone-mapped is passed over untouched and none of this applies.");

        {
        float wpScale = config->DlssNrWhitePointScale.value_or_default();
        if (ImGui::SliderFloat("Paper white", &wpScale, 0.25f, 4.0f, "%.2fx"))
            config->DlssNrWhitePointScale = wpScale;

        HelpMarker("Multiplies the white point above -- automatic or manual -- before the model"
                       "\nsees the frame. This is the paper-white control."
                       "\n\nAbove 1 the picture handed over is darker, so highlights sit lower on the"
                       "\ncurve and the model treats them as less extreme; below 1, the opposite. It"
                       "\nis the quickest way to change how strongly the model reads a bright scene."
                       "\n\nAt strength zero the frame is still bit-identical whatever this says.");

        float maxRatio = config->DlssNrMaxRatio.value_or_default();
        if (ImGui::SliderFloat("Highlight guard", &maxRatio, 1.0f, 8.0f, "%.1fx"))
            config->DlssNrMaxRatio = maxRatio;

        HelpMarker("The most the pass may brighten any pixel, as a multiple of what it already"
                       "\nwas. Darkening is not capped by this -- only growth is."
                       "\n\nLights are where the model has least to say and where rescaling its answer"
                       "\ninto the frame does the most damage: an early version turned every strip light"
                       "\nin the scene into a string of coloured cells. 2x leaves detail intact while"
                       "\nmaking that failure impossible. Raise it only if bright areas look clipped.");

        }

        ImGui::SeparatorText("Inspect");

        if (DlssNr::CaptureInProgress())
        {
            ImGui::TextDisabled("Capturing...");
        }
        else if (ImGui::Button("Capture 8 frames"))
        {
            DlssNr::RequestCapture(8);
        }

        HelpMarker("Writes eight consecutive frames twice: as the upscaler produced them, and again"
                       "\nonce the model's edit was applied."
                       "\n\nSame frames, same run, one variable -- which is what comparing two video"
                       "\ncaptures can never be, since they have different camera paths and a codec in"
                       "\nbetween that discards exactly the fine temporal detail in question."
                       "\n\nRaw, into a dlssnr-capture folder beside OptiScaler. Bounded to eight frames,"
                       "\nand each run overwrites the last.");

        static const char* debugNames[] = { "Off", "Proxy (what the model sees)", "Model output (raw)",
                                            "Difference (amplified)" };
        int debugView = (int) config->DlssNrDebugView.value_or_default();
        if (ImGui::Combo("Debug view", &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
            config->DlssNrDebugView = (uint32_t) debugView;

        HelpMarker("Proxy is the picture handed to the model -- if that looks wrong, the white point"
                       "\nis wrong and nothing downstream can be judged."
                       "\n\nDifference shows what the model actually changed, amplified twenty times and"
                       "\ncentred on grey. A flat grey frame there means it is doing nothing.");

        ImGui::PopItemWidth();
    }
}

} // namespace DlssNr

#endif // OPTI_DLSSNR
