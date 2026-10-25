#include "FileTypes/Classify.h"

#include "FileTypes/Extensions.h"
#include "FileTypes/FileName.h"
#include "FileTypes/Sniff.h"

namespace fastlook::core
{
    PreviewerKind ClassifyPreview(ClassifyInput const& in)
    {
        if (in.isVirtual || in.isCloudPlaceholder)
            return PreviewerKind::Fallback;
        if (in.isDirectory)
            return PreviewerKind::Folder;

        bool sniffText = LooksLikeText(in.head);
        bool skipToShell = false;

        if (auto byExt = KindForExtension(in.fileName))
        {
            PreviewerKind kind = *byExt;
            // ".ts" is both TypeScript and MPEG transport stream: the content decides.
            if (kind == PreviewerKind::Media && LowerExtension(in.fileName) == L"ts" && !in.head.empty() && sniffText)
                return PreviewerKind::Text;
            if (kind != PreviewerKind::Text && kind != PreviewerKind::Markdown)
                return kind;
            if (sniffText)
                return kind;
            skipToShell = true; // binary content under a text name: only a shell handler may take it
        }

        if (!skipToShell)
        {
            if (in.perceived == PerceivedKind::Image || in.hasWicDecoder)
                return PreviewerKind::Image;
            if (in.perceived == PerceivedKind::Audio || in.perceived == PerceivedKind::Video)
                return PreviewerKind::Media;
            if (in.perceived == PerceivedKind::Text || sniffText)
                return PreviewerKind::Text;
        }

        if (in.useShellHandlers && in.hasShellHandler)
            return PreviewerKind::Shell;
        return PreviewerKind::Fallback;
    }
}
