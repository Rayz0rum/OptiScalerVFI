#include <pch.h>

#include "DlssNr_Switch.h"

#if OPTI_DLSSNR

#include "DlssNr_Modes.h"

#include <Config.h>

namespace DlssNr
{

Mode ConfiguredMode()
{
    const auto raw = Config::Instance()->DlssNrMode.value_or_default();

    // A value from a hand-edited ini that names no mode is post-process, not undefined behaviour.
    if (raw > (uint32_t) Mode::MultiPassCustom)
        return Mode::PostProcess;

    return (Mode) raw;
}

} // namespace DlssNr

#endif // OPTI_DLSSNR
