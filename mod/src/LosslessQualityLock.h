#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace ammod::quality {

using SubtypeFactory = std::int32_t(__cdecl*)(void*, void*, void*, void**);
using LogFunction = void(*)(const std::wstring&);

bool Install(HMODULE coreMedia, HMODULE coreFoundation, LogFunction log);
void SetEnabled(bool enabled) noexcept;
std::int32_t CreateStrictLosslessFilter(SubtypeFactory factory, void* allocator,
                                        void* allowedSubtypes, std::uint16_t tier,
                                        void** output);

} // namespace ammod::quality
