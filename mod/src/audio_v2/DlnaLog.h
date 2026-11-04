#pragma once

#include <string>

namespace ammod::audio_v2 {

// Shared diagnostic log for the DLNA/OpenHome path. Writes one prefixed line
// to %PUBLIC%\AMExclusive\am-exclusive-dlna.log with rotation. Safe to call
// from any thread. Both the sink and the embedded HTTP server use it so a
// single file explains the whole stream lifecycle.
void LogDlnaLine(const std::wstring& text);

} // namespace ammod::audio_v2
