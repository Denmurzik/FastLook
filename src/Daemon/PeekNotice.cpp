#include "PeekNotice.h"

#include "Diagnostics/Log.h"
#include "Peek/PeekDetect.h"
#include "Strings.h"

namespace fastlook::daemon
{
    bool ShowPeekNoticeIfConflicting(core::Settings const& settings, Tray& tray)
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
        tray.ShowBalloon(S(StrId::PeekTitle), S(StrId::PeekText));
        return true;
    }
}
