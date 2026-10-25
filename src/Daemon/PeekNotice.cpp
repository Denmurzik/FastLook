#include "PeekNotice.h"

#include "Diagnostics/Log.h"
#include "Peek/PeekDetect.h"
#include "Strings.h"

namespace fastlook::daemon
{
    bool DetectPeekConflict(core::Settings const& settings)
    {
        if (!settings.general.peekWarning)
        {
            return false;
        }
        if (!core::DetectPeekConflictOnDisk(settings.general.activationKey).conflict)
        {
            return false;
        }
        FL_LOG_INFO(L"peek conflict");
        return true;
    }

    void ShowPeekNotice(Tray& tray)
    {
        tray.ShowBalloon(S(StrId::PeekTitle), S(StrId::PeekText));
    }
}
