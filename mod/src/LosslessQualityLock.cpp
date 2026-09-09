#include "LosslessQualityLock.h"
#include "LosslessQualityPolicy.h"

#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace ammod::quality {
namespace {

// FigSimpleAlternateFilterCreate copies these five pointers, retains context,
// and serializes reset/visit/test per filter.
// Its comparator is an empty-result fallback, NOT a normal sorting operation.
using Visit = void(__cdecl*)(void*, void*, void*);
using Predicate = unsigned char(__cdecl*)(void*, void*, void*);
using Description = void*(__cdecl*)(void*, void*);
struct Callbacks {
    void(__cdecl* reset)(void*);
    Visit visit;
    Predicate accept;
    std::int64_t(__cdecl* fallbackCompare)(void*, void*, void*);
    Description description;
};
static_assert(sizeof(Callbacks) == 40);
using SimpleFactory = std::int32_t(__cdecl*)(void*, void*, std::uint32_t,
                                            const Callbacks*, void*, void**);
SimpleFactory originalCreate{};
using TreeApply = std::int32_t(__cdecl*)(void*, void*, void**, void*);
TreeApply originalTreeApply{};
std::uintptr_t mediaBase{};
double(__cdecl* maxSampleRate)(void*){};
void*(__cdecl* arrayCreate)(void*, const void**, std::int64_t, const void*){};
const void*(__cdecl* arrayValue)(const void*, std::int64_t){};
std::int64_t(__cdecl* arrayCount)(const void*){};
void*(__cdecl* dataCreate)(void*, std::int64_t){};
void(__cdecl* dataSetLength)(void*, std::int64_t){};
unsigned char*(__cdecl* dataBytes)(void*){};
std::int64_t(__cdecl* dataLength)(const void*){};
void(__cdecl* release)(const void*){};
const void* arrayCallbacks{};
LogFunction writeLog{};
std::mutex installMutex;
bool installed{};
std::atomic<bool> enabled{};
std::atomic<bool> hasStrictFilters{};
thread_local bool creatingStrictFilter{};
thread_local bool wrappedStrictFilter{};

struct State {
    Predicate nativeAccept{};
    Description nativeDescription{};
    volatile LONG bestRate{};
};

// The outer CFArray retains Apple's original CFData (including its custom
// deallocator) and our separate state CFData. No map keyed by a recycled pointer,
// callback ownership leak or manual disposal of Apple objects is required.
State* GetState(void* context) {
    return reinterpret_cast<State*>(dataBytes(const_cast<void*>(arrayValue(context, 1))));
}
void* NativeContext(void* context) {
    return const_cast<void*>(arrayValue(context, 0));
}

void __cdecl Observe(void* alternate, void* context, void* filter) {
    if (!enabled.load(std::memory_order_acquire)) return;
    auto& state = *GetState(context);
    const bool allowed = state.nativeAccept(alternate, NativeContext(context), filter) != 0;
    Selection candidate{192000, 0};
    candidate.Observe(allowed, allowed ? maxSampleRate(alternate) : 0.0);
    LONG previous = InterlockedCompareExchange(&state.bestRate, 0, 0);
    while (candidate.bestRate > static_cast<std::uint32_t>(previous)) {
        const LONG actual = InterlockedCompareExchange(&state.bestRate,
            static_cast<LONG>(candidate.bestRate), previous);
        if (actual != previous) { previous = actual; continue; }
        if (writeLog) {
            writeLog(L"Strict ALAC quality filter=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(filter)) +
                L" highestAvailableRate=" + std::to_wstring(candidate.bestRate) +
                L" ceiling=192000; bandwidth downgrade forbidden");
        }
        break;
    }
}

unsigned char __cdecl Accept(void* alternate, void* context, void* filter) {
    if (!enabled.load(std::memory_order_acquire)) return 1;
    auto& state = *GetState(context);
    const bool allowed = state.nativeAccept(alternate, NativeContext(context), filter) != 0;
    const Selection selection{192000, static_cast<std::uint32_t>(
        InterlockedCompareExchange(&state.bestRate, 0, 0))};
    return static_cast<unsigned char>(
        selection.Accept(allowed, allowed ? maxSampleRate(alternate) : 0.0));
}

void* __cdecl Describe(void* filter, void* context) {
    const auto& state = *GetState(context);
    return state.nativeDescription ? state.nativeDescription(filter, NativeContext(context)) : nullptr;
}

std::int32_t __cdecl Create(void* allocator, void* name, std::uint32_t priority,
                            const Callbacks* callbacks, void* context, void** output) {
    if (!creatingStrictFilter) {
        return originalCreate(allocator, name, priority, callbacks, context, output);
    }
    if (output) *output = nullptr;
    // Accept only the empirically verified AllowableMediaSubtypes constructor.
    // Unexpected ABI/callback shape fails the strict request, never returns the
    // old codec-only filter and silently claims highest-quality admission.
    if (!output || !context || !callbacks || priority != 0x375 ||
        callbacks->reset || callbacks->visit || !callbacks->accept || callbacks->fallbackCompare) {
        return -1;
    }
    const State state{callbacks->accept, callbacks->description, 0};
    void* stateData = dataCreate(allocator, sizeof(state));
    if (stateData) {
        dataSetLength(stateData, sizeof(state));
        auto* storage = dataBytes(stateData);
        if (!storage || dataLength(stateData) != sizeof(state)) {
            release(stateData);
            return -1;
        }
        std::memcpy(storage, &state, sizeof(state));
    }
    const void* values[]{context, stateData};
    void* retained = stateData ? arrayCreate(allocator, values, 2, arrayCallbacks) : nullptr;
    if (stateData) release(stateData);
    if (!retained) return -1;
    const Callbacks strict{nullptr, Observe, Accept, nullptr, Describe};
    const auto result = originalCreate(allocator, name, priority, &strict, retained, output);
    release(retained);
    wrappedStrictFilter = result == 0 && *output;
    if (wrappedStrictFilter) hasStrictFilters.store(true, std::memory_order_release);
    return result;
}

// Read only the fingerprint-verified native filter layout. Class methods identify
// simple vs tree objects before reading their different storage. No Apple object
// is mutated. The tree's own security/availability filters still execute first;
// our final intersection can only REMOVE results, never make a rejected stream
// playable. Its fallback is otherwise able to reintroduce lower ALAC candidates.
std::uintptr_t Word(void* object, std::size_t offset) {
    std::uintptr_t value{};
    std::memcpy(&value, static_cast<unsigned char*>(object) + offset, sizeof(value));
    return value;
}
bool CollectStrict(void* filter, std::vector<void*>& strict, unsigned depth = 0) {
    if (!filter) return true;
    if (depth > 32 || strict.size() > 4096) return false;
    auto* table = reinterpret_cast<void*>(Word(filter, 0x18));
    if (!table) return true;
    auto* methods = reinterpret_cast<void*>(Word(table, 0x10));
    if (!methods) return true;
    if (Word(methods, 0x18) == mediaBase + 0x6ca8e0) {
        if (Word(filter, 0x40) == reinterpret_cast<std::uintptr_t>(&Accept))
            strict.push_back(filter);
    } else if (Word(methods, 0x20) == mediaBase + 0x6e2f10) {
        auto* children = reinterpret_cast<void*>(Word(filter, 0x30));
        const auto count = children ? arrayCount(children) : 0;
        if (count < 0 || count > 4096) return false;
        for (std::int64_t i = 0; i < count; ++i) {
            if (!CollectStrict(const_cast<void*>(arrayValue(children, i)), strict, depth + 1)) return false;
        }
        return CollectStrict(reinterpret_cast<void*>(Word(filter, 0x38)), strict, depth + 1);
    }
    return true;
}

std::int32_t __cdecl ApplyTree(void* tree, void* input, void** output, void* info) {
    if (!enabled.load(std::memory_order_acquire) ||
        !hasStrictFilters.load(std::memory_order_acquire))
        return originalTreeApply(tree, input, output, info);
    try {
        std::vector<void*> strict;
        if (!CollectStrict(tree, strict)) return -1;
        if (strict.empty()) return originalTreeApply(tree, input, output, info);
        // Establish the source-quality ceiling from this item's original list,
        // before bandwidth leaves can remove 96k and present only 48k downstream.
        const auto count = input ? arrayCount(input) : 0;
        if (count < 0 || count > 65536) return -1;
        for (void* filter : strict) {
            void* context = reinterpret_cast<void*>(Word(filter, 0x68));
            for (std::int64_t i = 0; i < count; ++i)
                Observe(const_cast<void*>(arrayValue(input, i)), context, filter);
        }
        const auto result = originalTreeApply(tree, input, output, info);
        if (result != 0 || !output || !*output) return result;
        const auto returned = arrayCount(*output);
        std::vector<const void*> kept;
        for (std::int64_t i = 0; i < returned; ++i) {
            void* alternate = const_cast<void*>(arrayValue(*output, i));
            bool admitted = true;
            for (void* filter : strict) {
                if (!Accept(alternate, reinterpret_cast<void*>(Word(filter, 0x68)), filter)) {
                    admitted = false;
                    break;
                }
            }
            if (admitted) kept.push_back(alternate);
        }
        if (kept.size() == static_cast<std::size_t>(returned)) return result;
        void* constrained = arrayCreate(nullptr, kept.data(), static_cast<std::int64_t>(kept.size()), arrayCallbacks);
        release(*output);
        *output = constrained;
        if (writeLog) writeLog(L"Strict ALAC tree rejected bandwidth/fallback downgrade count=" +
            std::to_wstring(returned - static_cast<std::int64_t>(kept.size())));
        return constrained ? 0 : -1;
    } catch (...) {
        if (output && *output) { release(*output); *output = nullptr; }
        return -1;
    }
}

} // namespace

bool Install(HMODULE media, HMODULE foundation, LogFunction log) {
    std::lock_guard lock(installMutex);
    if (installed) return true;
    writeLog = log;
    maxSampleRate = reinterpret_cast<decltype(maxSampleRate)>(GetProcAddress(media, "FigAlternateGetMaxAudioSampleRate"));
    arrayCreate = reinterpret_cast<decltype(arrayCreate)>(GetProcAddress(foundation, "CFArrayCreate"));
    arrayValue = reinterpret_cast<decltype(arrayValue)>(GetProcAddress(foundation, "CFArrayGetValueAtIndex"));
    arrayCount = reinterpret_cast<decltype(arrayCount)>(GetProcAddress(foundation, "CFArrayGetCount"));
    dataCreate = reinterpret_cast<decltype(dataCreate)>(GetProcAddress(foundation, "CFDataCreateMutable"));
    dataSetLength = reinterpret_cast<decltype(dataSetLength)>(GetProcAddress(foundation, "CFDataSetLength"));
    dataBytes = reinterpret_cast<decltype(dataBytes)>(GetProcAddress(foundation, "CFDataGetMutableBytePtr"));
    dataLength = reinterpret_cast<decltype(dataLength)>(GetProcAddress(foundation, "CFDataGetLength"));
    release = reinterpret_cast<decltype(release)>(GetProcAddress(foundation, "CFRelease"));
    arrayCallbacks = GetProcAddress(foundation, "kCFTypeArrayCallBacks");
    auto target = reinterpret_cast<void*>(GetProcAddress(media, "FigSimpleAlternateFilterCreate"));
    if (!maxSampleRate || !arrayCreate || !arrayValue || !arrayCount || !dataCreate ||
        !dataSetLength || !dataBytes || !dataLength ||
        !release || !arrayCallbacks || !target) return false;
    if (!originalCreate && MH_CreateHook(target, reinterpret_cast<void*>(Create),
                                        reinterpret_cast<void**>(&originalCreate)) != MH_OK) return false;
    const auto status = MH_EnableHook(target);
    if (status != MH_OK && status != MH_ERROR_ENABLED) return false;
    mediaBase = reinterpret_cast<std::uintptr_t>(media);
    auto* treeTarget = reinterpret_cast<void*>(mediaBase + 0x6e2f10);
    if (!originalTreeApply && MH_CreateHook(treeTarget, reinterpret_cast<void*>(ApplyTree),
        reinterpret_cast<void**>(&originalTreeApply)) != MH_OK) return false;
    const auto treeStatus = MH_EnableHook(treeTarget);
    installed = treeStatus == MH_OK || treeStatus == MH_ERROR_ENABLED;
    if (installed && writeLog) writeLog(L"Highest ALAC quality filter hook ready");
    return installed;
}

void SetEnabled(bool value) noexcept {
    enabled.store(value, std::memory_order_release);
}

std::int32_t CreateHighestLosslessFilter(SubtypeFactory factory, void* allocator,
                                        void* allowedSubtypes, void** output) {
    if (output) *output = nullptr;
    if (!enabled.load(std::memory_order_acquire) || !installed || !factory || !output ||
        creatingStrictFilter) return -1;
    struct Scope {
        Scope() { creatingStrictFilter = true; wrappedStrictFilter = false; }
        ~Scope() { creatingStrictFilter = false; wrappedStrictFilter = false; }
    } scope;
    const auto result = factory(allocator, allowedSubtypes, nullptr, output);
    if (result != 0 || !wrappedStrictFilter) {
        if (*output) release(*output);
        *output = nullptr;
        return result != 0 ? result : -1;
    }
    return result;
}

} // namespace ammod::quality
