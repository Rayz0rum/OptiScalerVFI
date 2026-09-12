#include "pch.h"

#include "MfgUnlock.h"

#include <Config.h>
#include <State.h>
#include <hooks/Streamline_Hooks.h>
#include <menu/menu_common.h>
#include <framegen/dlssg/AmpereMfgLoader.h>

#include <imgui/imgui.h>

namespace MfgUnlock
{

namespace
{

// The "(?)" marker every control carries, matching the rest of the menu.
void HelpMarker(const char* tip)
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

} // namespace

/*
 * Turing and Ampere, which get there by a different route entirely.
 *
 * Its own header rather than a checkbox inside the Ada one, because nothing about the two is shared:
 * Ada rewrites a comparison inside NVIDIA's snippet, this sideloads a third-party build of frame
 * generation compiled for architectures NVIDIA never shipped one for. They are mutually exclusive and
 * the loader refuses to start if both are on.
 */
static void RenderAmpereSection(Config* config)
{
    ImGui::Spacing();

    if (auto ch = ScopedCollapsingHeader("DLSS Multi Frame Generation (RTX 20 / 30 sideload)");
        !ch.IsHeaderOpen())
        return;

    ScopedIndent indent {};
    ImGui::Spacing();

    const auto status = AmpereMfgLoader::LastStatus();

    if (!status.ErrorMessage.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", status.ErrorMessage.c_str());
    else if (status.FsrFallbackActive)
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "Falling back to OptiScaler's own FSR frame generation.");
    else if (status.DllLoaded)
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "dlssg_sm86.dll loaded.");
    else if (status.Enabled && !status.DllFound)
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f),
                           "dlssg_sm86.dll not found - put it in OptiScaler/dlssg_sm86/.");
    else if (!status.Enabled)
        ImGui::TextDisabled("Off. Takes effect at startup.");

    bool enabled = config->FGDLSSGAmpereMfgUnlock.value_or_default();
    if (ImGui::Checkbox("Enable RTX 20/30 multi-frame generation", &enabled))
        config->FGDLSSGAmpereMfgUnlock = enabled;

    HelpMarker("Sideloads a third-party build of DLSS Frame Generation compiled for Turing and"
                   "\nAmpere, and writes the companion ini it expects."
                   "\n\nThis is not a patch of NVIDIA's snippet and could not be. The Ada unlock"
                   "\nabove rewrites a Blackwell comparison inside nvngx_dlssg.dll; Turing and"
                   "\nAmpere are excluded a step earlier, by the snippet's own hardcoded minimum"
                   "\narchitecture of 0x190 (Ada). Lowering that would hand the feature hardware"
                   "\nNVIDIA never built an implementation for -- so a separate implementation is"
                   "\nloaded instead."
                   "\n\nYOU supply dlssg_sm86.dll. It is not ours and is not shipped. Put it in"
                   "\nOptiScaler/dlssg_sm86/ beside the OptiScaler folder."
                   "\n\nMutually exclusive with the RTX 40 unlock, and applied at startup, so a"
                   "\nchange here needs a restart.");

    int frames = config->FGDLSSGAmpereMfgMaxFrames.value_or_default();
    if (ImGui::SliderInt("Generated frames", &frames, 1, 3, "%d"))
        config->FGDLSSGAmpereMfgMaxFrames = frames;

    HelpMarker("Generated frames, not total: 1 is 2x, 2 is 3x, 3 is 4x.");

    static const char* kernelNames[] = { "Auto", "PTX", "Cubin" };
    const std::string current = config->FGDLSSGAmpereMfgKernelImage.value_or("Auto");
    int kernel = current == "PTX" ? 1 : (current == "Cubin" ? 2 : 0);

    if (ImGui::Combo("Kernel image", &kernel, kernelNames, IM_ARRAYSIZE(kernelNames)))
        config->FGDLSSGAmpereMfgKernelImage = kernelNames[kernel];

    HelpMarker("How the interpolation kernel is supplied. Auto picks PTX on Turing and on"
                   "\nLinux, Cubin otherwise, which is the right answer unless you are"
                   "\ntroubleshooting.");

    bool bilinear = config->FGDLSSGAmpereMfgHardwareBilinear.value_or_default();
    if (ImGui::Checkbox("Hardware bilinear", &bilinear))
        config->FGDLSSGAmpereMfgHardwareBilinear = bilinear;

    HelpMarker("Approximate sampling. Ampere only, and off by default -- it trades a little"
                   "\naccuracy for speed.");

    ImGui::Spacing();
}

void RenderMenu(Config* config, float menuResScale)
{
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("DLSS Multi Frame Generation (RTX 40 unlock)"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        auto& telemetry = GetTelemetry();
        const Status status = GetStatus();

        bool enabled = config->MfgUnlockEnabled.value_or_default();
        if (ImGui::Checkbox("Enable multi-frame unlock (Ada)", &enabled))
        {
            config->MfgUnlockEnabled = enabled;
            ApplyConfig();
        }
        HelpMarker("Rewrites, in memory only, the two places where nvngx_dlssg.dll checks for an RTX 50 before "
                   "advertising and generating more than one frame, so the game's own DLSS Frame Generation can "
                   "run 3x/4x (and up to 6x where the Streamline plugin allows) on an RTX 40.\n\n"
                   "Nothing on disk is modified. NVIDIA verifies the file's signature when it loads it, not "
                   "afterwards.\n\nApplied when the DLL is mapped: enable this, save, and restart the game. Leave "
                   "OptiScaler's own Frame Generation off -- the game's DLSS-G does the work here.");

        if (enabled && telemetry.skippedNotAda.load(std::memory_order_relaxed))
        {
            const auto arch = telemetry.detectedArch.load(std::memory_order_relaxed);
            const auto devId = telemetry.detectedDeviceId.load(std::memory_order_relaxed);
            if (arch != 0)
                ImGui::TextWrapped("Inactive: Streamline reports architecture 0x%x, which does not need it.", arch);
            else
                ImGui::TextWrapped("Inactive: device id 0x%x is not recognised as an RTX 40-series part.", devId);
        }

        ImGui::Separator();

        // Force multiplier ---------------------------------------------------
        int force = static_cast<int>(ForceMultiplier());
        ImGui::PushItemWidth(180.0f * menuResScale);
        if (ImGui::SliderInt("Force frame multiplier", &force, 0, 6, force == 0 ? "off (game decides)" : "%dx"))
        {
            if (force != 0 && force < 2)
                force = 2;

            config->MfgUnlockForceMultiplier = force;
            ApplyConfig();
            StreamlineHooks::updateDlssgOptions();
        }
        ImGui::PopItemWidth();
        HelpMarker("Leave off for games with their own 2x/3x/4x selector -- forcing would override your in-game "
                   "choice. Use it where frame generation is only on/off: the game keeps asking for 2x and this "
                   "raises the request on its way to Streamline. 6x is the plugin's own hard ceiling; a refusal "
                   "is logged and the game's request goes through unchanged.");

        if (telemetry.intercepted.load(std::memory_order_relaxed))
        {
            ImGui::Text("Game asked for %ux, forced to %u generated frame(s).",
                        telemetry.lastRequested.load(std::memory_order_relaxed) + 1,
                        telemetry.lastForced.load(std::memory_order_relaxed));
        }
        else if (telemetry.declinedNoPacing.load(std::memory_order_relaxed))
        {
            ImGui::TextWrapped("Declined to force: flip metering was still on when DLSS-G started.");
        }
        else if (telemetry.nativeRequestSeen.load(std::memory_order_relaxed))
        {
            ImGui::TextDisabled("Game requests %ux itself (sl::Result %u).",
                                telemetry.nativeRequested.load(std::memory_order_relaxed) + 1,
                                telemetry.nativeResult.load(std::memory_order_relaxed));
        }
        else
        {
            ImGui::TextDisabled("slDLSSGSetOptions not seen yet -- turn the game's DLSS Frame Generation on.");
        }

        ImGui::Separator();

        // Patch status -------------------------------------------------------
        if (status.gatePatched)
            ImGui::Text("DLSS-G arch gates rewritten: %zu provider(s), %zu site(s).", status.gateModules,
                        status.gateSites);
        else
            ImGui::TextDisabled("DLSS-G snippet not patched yet (%u provider(s) seen) -- enable frame generation.",
                                status.providersSeen);

        bool temporal = config->MfgUnlockTemporalFix.value_or_default();
        if (ImGui::Checkbox("Temporal fix (frames land at their own time, not all at the midpoint)", &temporal))
        {
            config->MfgUnlockTemporalFix = temporal;
            ApplyConfig();
        }
        HelpMarker("The interpolation kernel shipped for Ada blends the two source frames with a constant 0.5, so "
                   "3x/4x would show identical half-way frames. This rewrites the kernel's PTX so the blend weight "
                   "comes from its temporal parameter. Applied once when the snippet loads; restart the game to "
                   "change it.");

        if (status.midpointPatched)
            ImGui::TextWrapped("Temporal fix: %s.", status.midpointDetail.c_str());
        else
            ImGui::TextDisabled("Temporal fix not applied yet (attempt %d).", status.midpointAttempts);

        ImGui::Separator();

        // Pacing -------------------------------------------------------------
        bool flipOff = config->MfgUnlockForceFlipMeterOff.value_or_default();
        if (ImGui::Checkbox("Force legacy software flip pacing (compatibility)", &flipOff))
        {
            config->MfgUnlockForceFlipMeterOff = flipOff;
            ApplyConfig();
        }
        HelpMarker("Blackwell paces multi-frame output with hardware flip metering that Ada does not have; left "
                   "enabled, 3x/4x can freeze the picture while audio keeps running. This pins the Streamline "
                   "plugin onto its software pacer. Leave off with current Streamline builds; enable only if "
                   "3x/4x freezes. Forcing a multiplier above 2x applies it on demand anyway.");

        if (status.flipMeterPatched)
        {
            ImGui::Text("Flip-metering forced off: +0x%x pinned to %u, %zu site(s).", status.flipMeterOffset,
                        status.flipMeterValue, status.flipMeterSites);
        }
        else if (status.flipMeterGaveUp)
        {
            ImGui::TextDisabled("DLSS-G plugin found, but flip metering could not be patched (see the log).");
        }
        else if (status.flipMeterAttempts > 0)
        {
            ImGui::TextDisabled("DLSS-G plugin found; flip-metering patch pending (%d).", status.flipMeterAttempts);
        }
        else
        {
            ImGui::TextDisabled("Streamline DLSS-G plugin not patched yet.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Apply now"))
                TryPatchFlipMetering();
        }

        if (status.ceilingPatched)
            ImGui::Text("Streamline device-limit bypassed: compiled %ux, effective %ux.", status.ceilingCompiled + 1,
                        status.ceilingEffective + 1);

        bool raise = config->MfgUnlockRaiseCeiling.value_or_default();
        if (ImGui::Checkbox("Raise the plugin's compiled frame ceiling to 6x", &raise))
        {
            config->MfgUnlockRaiseCeiling = raise;
            ApplyConfig();
        }
        HelpMarker("Old sl.dlss_g builds are compiled with a ceiling of 3 generated frames (4x). Lifting it is "
                   "not the same as the plugin being able to cope -- it broke GTA V Enhanced. Off by default; "
                   "updating the plugin is the sound fix. Read when the pacing patch is applied.");

        bool ota = config->MfgUnlockForceOta.value_or_default();
        if (ImGui::Checkbox("Re-enable Streamline OTA plugins at slInit", &ota))
        {
            config->MfgUnlockForceOta = ota;
            ApplyConfig();
        }
        HelpMarker("Some games drop the flags that let Streamline load the driver's downloaded (newer) plugin set. "
                   "Putting them back means a matched, signed, current sl.dlss_g -- but a newer plugin can also "
                   "crash a game that was never tested with it. Off by default; needs a restart.");

        ImGui::Separator();

        // Streamline telemetry ----------------------------------------------
        if (telemetry.capacityAdvertised.load(std::memory_order_relaxed))
        {
            ImGui::Text("Native menu maximum: runtime %ux, advertised %ux.",
                        telemetry.runtimeMaxGenerated.load(std::memory_order_relaxed) + 1,
                        AdvertisedMaxGenerated() + 1);
        }
        else if (telemetry.runtimeMaxGenerated.load(std::memory_order_relaxed) > 0)
        {
            ImGui::Text("Streamline reports up to %ux to the game.",
                        telemetry.runtimeMaxGenerated.load(std::memory_order_relaxed) + 1);
        }

        if (telemetry.stateSeen.load(std::memory_order_relaxed))
        {
            const unsigned int dlssgStatus = telemetry.dlssgStatus.load(std::memory_order_relaxed);
            ImGui::Text("Actual presentations since last state query: %u (samples: %llu).",
                        telemetry.actualPresented.load(std::memory_order_relaxed),
                        telemetry.stateSamples.load(std::memory_order_relaxed));

            if (dlssgStatus != 0)
            {
                ImGui::Text("DLSS-G runtime status: failure flags 0x%x.", dlssgStatus);
            }
            else
            {
                const unsigned int observed = telemetry.maxActualPresented.load(std::memory_order_relaxed);
                if (observed > 1)
                    ImGui::Text("MFG validation: active; observed up to %u actual presentations.", observed);
                else
                    ImGui::TextDisabled("DLSS-G status is OK; generated output has not been confirmed yet.");
            }
        }
        else
        {
            ImGui::TextDisabled("No successful slDLSSGGetState telemetry sample yet (last result: %u).",
                                telemetry.stateResult.load(std::memory_order_relaxed));
        }

        if (telemetry.otaForced.load(std::memory_order_relaxed))
            ImGui::TextDisabled("slInit OTA flags were forced on (were 0x%llx).",
                                telemetry.otaFlagsBefore.load(std::memory_order_relaxed));

        ImGui::Spacing();
    }

    RenderAmpereSection(config);
}

} // namespace MfgUnlock
