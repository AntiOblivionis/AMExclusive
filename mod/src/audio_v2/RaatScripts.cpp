#include "RaatScripts.h"

#include <windows.h>

#include <cstring>

namespace ammod::raat {
namespace {

constexpr ScriptRecord kOfficial[] = {
    {R"(raat.dat:base.lua)", "base",
     "219d85674e22949fb629bcb9ef300d0ba53bda5ddc0bdc36961666134fa89865d07619085cd5feafce45ed49721e1cf3d4de7fde8b7bf5e4efed0d3bc762b800",
     3001, true},
    {R"(raat.dat:dkjson.lua)", "dkjson",
     "019bc04a8211c4e9b7bb0f127d5a00bcd59cc76d9b2cdb8c31a2afe87dd5adc4b2bceb488514a5261938d126cca4b92bfcb95d6f4fa91fd45a68022ae4065a05",
     3002, true},
    {R"(raat.dat:protocol.lua)", "protocol",
     "bdc729ea2ce13cc7574b66ad396e2b7fef98a3887ad3184804467abcc003f943268aebf438a1709646cf39cd8186ba03514572986fbd4444b931c15e486f9f0e",
     3003, true},
    {R"(raat.dat:roon_tcp.lua)", "roon_tcp",
     "56cf6c4d5311160d3eb0bb2efccf9e4d58bc69edb6a1bf213aa3d784c9a99ddaeeb39a90dcd926bf294ad58a291e14b1fe40afe58118d089e6754874ef4a0f0b",
     3004, true},
};

constexpr ScriptRecord kCleanroom[] = {
    {"rbase.lua", "rbase", "", 3005, false},
    {"rjson.lua", "rjson", "", 3006, false},
    {"rprotocol.lua", "rprotocol", "", 3007, false},
    // The main script must NOT declare a module: the renderer executes it
    // (it ends with go()/return roon) instead of caching it as a library.
    // A captured working foobar2000 session sends rroon.lua without "module";
    // adding one makes the device cache the main script and never install the
    // dispatcher, so every later request returns NotImplemented.
    {"rroon.lua", "", "", 3008, false},
};

HMODULE ThisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LoadScriptResource), &module);
    return module;
}

} // namespace

const ScriptRecord* OfficialScripts(std::size_t& count) noexcept {
    count = sizeof(kOfficial) / sizeof(kOfficial[0]);
    return kOfficial;
}

const ScriptRecord* CleanroomScripts(std::size_t& count) noexcept {
    count = sizeof(kCleanroom) / sizeof(kCleanroom[0]);
    return kCleanroom;
}

std::string LoadScriptResource(int resourceId) {
    const HMODULE module = ThisModule();
    if (!module) return {};
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) return {};
    const HGLOBAL loaded = LoadResource(module, resource);
    if (!loaded) return {};
    const DWORD size = SizeofResource(module, resource);
    const void* data = LockResource(loaded);
    if (!data || size == 0) return {};
    return std::string(static_cast<const char*>(data), static_cast<std::size_t>(size));
}

} // namespace ammod::raat
