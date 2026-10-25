#pragma once

#include <memory>
#include <string_view>

#include "Previewers/Text/ILineStyler.h"
#include "Text/TextDocument.h"

namespace fastlook::viewer
{
    // Syntax styling of a text document with a Lexilla lexer, run on a background thread from the start of the file down to the
    // last visible line plus a margin. nullptr (no highlighting) when `lexer` is empty, the file is over 8 MiB or has over 500 000 lines or UTF-16, or its
    // encoding is unsupported. The worker starts on the first EnsureStyledThrough and is stopped by the destructor; `invalidated`
    // may be called from it.
    std::shared_ptr<ILineStyler> MakeLexillaStyler(std::shared_ptr<core::TextDocument> document, std::string_view lexer);
}
