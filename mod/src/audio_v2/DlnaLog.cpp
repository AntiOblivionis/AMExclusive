#include "DlnaLog.h"

#include "../BoundedLog.h"

#include <windows.h>

#include <filesystem>
#include <iterator>
#include <mutex>

namespace ammod::audio_v2 {
namespace {

std::mutex g_dlnaLogMutex;

} // namespace

void LogDlnaLine(const std::wstring& text) {
    wchar_t publicDir[MAX_PATH]{};
    GetEnvironmentVariableW(L"PUBLIC", publicDir,
                            static_cast<DWORD>(std::size(publicDir)));
    std::filesystem::path path;
    if (publicDir[0]) {
        path = std::filesystem::path(publicDir) / L"AMExclusive" /
            L"am-exclusive-dlna.log";
    } else {
        wchar_t modulePath[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, modulePath, MAX_PATH)) return;
        path = std::filesystem::path(modulePath).parent_path() /
            L"am-exclusive-dlna.log";
    }
    std::wstring line = L"dlna ";
    line += text;
    ammod::logging::AppendWideLine(path, g_dlnaLogMutex, line, 2ull * 1024ull * 1024ull, 2);
}

} // namespace ammod::audio_v2
