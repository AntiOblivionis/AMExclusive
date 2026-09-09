#pragma once

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>

namespace ammod::logging {

inline std::filesystem::path BackupPath(const std::filesystem::path& path,
                                        unsigned index) {
    return std::filesystem::path(path.wstring() + L"." + std::to_wstring(index));
}

inline void RotateIfNeeded(const std::filesystem::path& path,
                           std::uintmax_t maxBytes,
                           unsigned backups) noexcept {
    if (maxBytes == 0 || backups == 0) return;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < maxBytes) return;

    for (unsigned index = backups; index > 0; --index) {
        const auto destination = BackupPath(path, index);
        std::filesystem::remove(destination, ec);
        ec.clear();
        const auto source = index == 1 ? path : BackupPath(path, index - 1);
        if (!std::filesystem::exists(source, ec) || ec) {
            ec.clear();
            continue;
        }
        std::filesystem::rename(source, destination, ec);
        ec.clear();
    }
}

inline std::string Utf8(std::wstring_view value) noexcept {
    if (value.empty() || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int sourceLength = static_cast<int>(value.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), sourceLength,
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(), sourceLength,
                            result.data(), bytes, nullptr, nullptr) != bytes) {
        return {};
    }
    return result;
}

inline void AppendWideLine(const std::filesystem::path& path,
                           std::mutex& mutex,
                           std::wstring_view line,
                           std::uintmax_t maxBytes,
                           unsigned backups = 2) noexcept {
    const auto utf8 = Utf8(line);
    if (utf8.empty() && !line.empty()) return;
    std::lock_guard lock(mutex);
    RotateIfNeeded(path, maxBytes, backups);
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream) return;
    stream.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    stream.put('\n');
}

} // namespace ammod::logging
