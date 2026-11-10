#pragma once

#include <cstddef>
#include <string>

namespace ammod::raat {

// Raw Lua payloads are compiled into the hook DLL as RCDATA so a single
// component file carries both the official signed raat.dat corpus and the
// clean-room replacement scripts.
struct ScriptRecord final {
    const char* name;
    const char* module;
    const char* signature; // empty for clean-room scripts
    int resourceId;
    bool official;
};

const ScriptRecord* OfficialScripts(std::size_t& count) noexcept;
const ScriptRecord* CleanroomScripts(std::size_t& count) noexcept;

// Returns the resource bytes, or an empty string if the resource is missing.
std::string LoadScriptResource(int resourceId);

} // namespace ammod::raat
