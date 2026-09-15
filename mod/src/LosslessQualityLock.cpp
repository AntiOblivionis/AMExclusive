#include "LosslessQualityLock.h"
#include "LosslessQualityPolicy.h"
#include "ApplePrivateOffsets.h"

#include <MinHook.h>
#include <algorithm>
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
std::atomic<std::uint64_t> policyGeneration{1};
std::atomic<bool> hasStrictFilters{};
thread_local bool creatingStrictFilter{};
thread_local bool wrappedStrictFilter{};
thread_local std::uint16_t creatingStrictTier{};

struct EvaluationState {
    volatile LONG sawLossless{};
    volatile LONG bestRate{};
    volatile LONG enabled{};
    volatile LONG64 generation{};
};

struct State {
    Predicate nativeAccept{};
    Description nativeDescription{};
    EvaluationState directEvaluation{};
    LONG tier{};
};

struct TreeEvaluationEntry {
    State* state{};
    EvaluationState evaluation{};
};

struct TreeEvaluationFrame {
    std::vector<TreeEvaluationEntry> entries;
};

thread_local std::vector<TreeEvaluationFrame*> treeEvaluationStack;

struct TreeEvaluationScope final {
    explicit TreeEvaluationScope(TreeEvaluationFrame& frame) : frame_(&frame) {
        treeEvaluationStack.push_back(frame_);
    }
    ~TreeEvaluationScope() {
        if (!treeEvaluationStack.empty() && treeEvaluationStack.back() == frame_) {
            treeEvaluationStack.pop_back();
        }
    }
    TreeEvaluationFrame* frame_{};
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

EvaluationState* FindTreeEvaluation(State* state) noexcept {
    for (auto frame = treeEvaluationStack.rbegin(); frame != treeEvaluationStack.rend(); ++frame) {
        for (auto& entry : (*frame)->entries) {
            if (entry.state == state) return &entry.evaluation;
        }
    }
    return nullptr;
}

EvaluationState& CurrentEvaluation(State& state) noexcept {
    if (auto* evaluation = FindTreeEvaluation(&state)) return *evaluation;
    return state.directEvaluation;
}

void ResetEvaluation(EvaluationState& evaluation, bool isEnabled,
                     std::uint64_t generation) noexcept {
    InterlockedExchange(&evaluation.sawLossless, 0);
    InterlockedExchange(&evaluation.bestRate, 0);
    InterlockedExchange(&evaluation.enabled, isEnabled ? 1L : 0L);
    InterlockedExchange64(&evaluation.generation, static_cast<LONG64>(generation));
}

void __cdecl Reset(void* context) {
    auto& state = *GetState(context);
    auto& evaluation = state.directEvaluation;
    const auto currentGeneration = policyGeneration.load(std::memory_order_acquire);
    const auto previousGeneration = static_cast<std::uint64_t>(
        InterlockedCompareExchange64(&evaluation.generation, 0, 0));
    // A standalone Simple filter may be reevaluated several times for the same
    // media item as bandwidth leaves change. Preserve its discovered ceiling
    // within one policy epoch; otherwise a later 48-only reevaluation can undo
    // a previously observed 96/192 candidate. A real policy transition starts
    // a fresh direct-filter epoch. Production tree evaluations do not use this
    // retained slot at all; ApplyTree owns a per-call thread-local frame.
    if (previousGeneration != currentGeneration) {
        InterlockedExchange(&evaluation.sawLossless, 0);
        InterlockedExchange(&evaluation.bestRate, 0);
        InterlockedExchange64(&evaluation.generation,
                              static_cast<LONG64>(currentGeneration));
    }
    InterlockedExchange(&evaluation.enabled,
                        enabled.load(std::memory_order_acquire) ? 1L : 0L);
}

void __cdecl Observe(void* alternate, void* context, void* filter) {
    auto& state = *GetState(context);
    auto& evaluation = CurrentEvaluation(state);
    if (InterlockedCompareExchange(&evaluation.enabled, 0, 0) == 0) return;
    const bool allowed = state.nativeAccept(alternate, NativeContext(context), filter) != 0;
    if (!allowed) return;
    InterlockedExchange(&evaluation.sawLossless, 1);
    if (state.tier != 20) return;
    Selection candidate{192000, 0};
    candidate.Observe(true, maxSampleRate(alternate));
    LONG previous = InterlockedCompareExchange(&evaluation.bestRate, 0, 0);
    while (candidate.bestRate > static_cast<std::uint32_t>(previous)) {
        const LONG actual = InterlockedCompareExchange(&evaluation.bestRate,
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
    auto& state = *GetState(context);
    auto& evaluation = CurrentEvaluation(state);
    if (InterlockedCompareExchange(&evaluation.enabled, 0, 0) == 0) return 1;
    const bool allowed = state.nativeAccept(alternate, NativeContext(context), filter) != 0;
    const bool sawLossless = InterlockedCompareExchange(&evaluation.sawLossless, 0, 0) != 0;
    // A strict request is codec-strict only when this item's original candidate
    // inventory actually contains ALAC/QLAC. AAC-only catalog items must remain
    // playable instead of producing an empty successful candidate set forever.
    if (!sawLossless) return 1;
    if (!allowed) return 0;
    if (state.tier == 15) return 1;
    const Selection selection{192000, static_cast<std::uint32_t>(
        InterlockedCompareExchange(&evaluation.bestRate, 0, 0))};
    return static_cast<unsigned char>(selection.Accept(true, maxSampleRate(alternate)));
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
    if (!output || !context || !callbacks ||
        priority != ammod::apple_private::core_media_filter::kStrictConstructorPriority ||
        callbacks->reset || callbacks->visit || !callbacks->accept || callbacks->fallbackCompare) {
        return -1;
    }
    if (creatingStrictTier != 15 && creatingStrictTier != 20) return -1;
    const State state{callbacks->accept, callbacks->description, {},
                      static_cast<LONG>(creatingStrictTier)};
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
    // CoreMedia Simple Apply calls reset exactly once before visiting/testing an
    // evaluation. Use that real boundary for direct-simple policy/item state.
    // Tree evaluations use a thread-local frame below so outer-tree priming is
    // not destroyed by the nested Simple Apply reset and concurrent trees never
    // share candidate state through the retained filter object.
    const Callbacks strict{Reset, Observe, Accept, nullptr, Describe};
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
    namespace offsets = ammod::apple_private::core_media_filter;
    auto* table = reinterpret_cast<void*>(Word(filter, offsets::kFilterClassTable));
    if (!table) return true;
    auto* methods = reinterpret_cast<void*>(Word(table, offsets::kClassTableMethods));
    if (!methods) return true;
    if (Word(methods, offsets::kSimpleApplySlot) == mediaBase + offsets::kSimpleApplyRva) {
        if (Word(filter, offsets::kSimplePredicate) == reinterpret_cast<std::uintptr_t>(&Accept))
            strict.push_back(filter);
    } else if (Word(methods, offsets::kTreeApplySlot) == mediaBase + offsets::kTreeApplyRva) {
        auto* children = reinterpret_cast<void*>(Word(filter, offsets::kTreeChildren));
        const auto count = children ? arrayCount(children) : 0;
        if (count < 0 || count > 4096) return false;
        for (std::int64_t i = 0; i < count; ++i) {
            if (!CollectStrict(const_cast<void*>(arrayValue(children, i)), strict, depth + 1)) return false;
        }
        return CollectStrict(reinterpret_cast<void*>(Word(filter, offsets::kTreeFallback)), strict, depth + 1);
    }
    return true;
}

std::int32_t __cdecl ApplyTree(void* tree, void* input, void** output, void* info) {
    if (!hasStrictFilters.load(std::memory_order_acquire))
        return originalTreeApply(tree, input, output, info);
    try {
        std::vector<void*> strict;
        if (!CollectStrict(tree, strict)) return -1;
        if (strict.empty()) return originalTreeApply(tree, input, output, info);

        // A filter object may outlive one media item and the same retained
        // filter may participate in overlapping tree evaluations. Keep tree
        // candidate state in a thread-local frame rather than in retained CF
        // state. Direct Simple Apply uses its native reset callback and the
        // separate State::directEvaluation slot.
        const bool evaluationEnabled = enabled.load(std::memory_order_acquire);
        const auto evaluationGeneration = policyGeneration.load(std::memory_order_acquire);
        TreeEvaluationFrame frame;
        frame.entries.reserve(strict.size());
        for (void* filter : strict) {
            void* context = reinterpret_cast<void*>(Word(
                filter, ammod::apple_private::core_media_filter::kStrictContext));
            auto* state = GetState(context);
            const bool alreadyPresent = std::any_of(
                frame.entries.begin(), frame.entries.end(),
                [state](const TreeEvaluationEntry& entry) { return entry.state == state; });
            if (!alreadyPresent) {
                frame.entries.push_back({state, {}});
                ResetEvaluation(frame.entries.back().evaluation,
                                evaluationEnabled, evaluationGeneration);
            }
        }
        TreeEvaluationScope evaluationScope(frame);
        if (!evaluationEnabled) return originalTreeApply(tree, input, output, info);

        // Establish the source-quality ceiling from this evaluation's original
        // list before bandwidth leaves can remove high-rate ALAC candidates.
        const auto count = input ? arrayCount(input) : 0;
        if (count < 0 || count > 65536) return -1;
        for (void* filter : strict) {
            void* context = reinterpret_cast<void*>(Word(filter, ammod::apple_private::core_media_filter::kStrictContext));
            for (std::int64_t i = 0; i < count; ++i)
                Observe(const_cast<void*>(arrayValue(input, i)), context, filter);
            auto& state = *GetState(context);
            auto& evaluation = CurrentEvaluation(state);
            if (InterlockedCompareExchange(&evaluation.sawLossless, 0, 0) == 0 && writeLog) {
                writeLog(L"Strict ALAC filter found no lossless candidate in current evaluation; preserving AAC-only native selection");
            }
        }
        const auto result = originalTreeApply(tree, input, output, info);
        if (result != 0 || !output || !*output) return result;
        const auto returned = arrayCount(*output);
        std::vector<const void*> kept;
        for (std::int64_t i = 0; i < returned; ++i) {
            void* alternate = const_cast<void*>(arrayValue(*output, i));
            bool admitted = true;
            for (void* filter : strict) {
                if (!Accept(alternate, reinterpret_cast<void*>(Word(filter, ammod::apple_private::core_media_filter::kStrictContext)), filter)) {
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
    auto* treeTarget = reinterpret_cast<void*>(mediaBase +
        ammod::apple_private::core_media_filter::kTreeApplyRva);
    if (!originalTreeApply && MH_CreateHook(treeTarget, reinterpret_cast<void*>(ApplyTree),
        reinterpret_cast<void**>(&originalTreeApply)) != MH_OK) return false;
    const auto treeStatus = MH_EnableHook(treeTarget);
    installed = treeStatus == MH_OK || treeStatus == MH_ERROR_ENABLED;
    if (installed && writeLog) writeLog(L"Highest ALAC quality filter hook ready");
    return installed;
}

void SetEnabled(bool value) noexcept {
    const bool previous = enabled.exchange(value, std::memory_order_acq_rel);
    if (previous != value) {
        policyGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
}

std::int32_t CreateStrictLosslessFilter(SubtypeFactory factory, void* allocator,
                                        void* allowedSubtypes, std::uint16_t tier,
                                        void** output) {
    if (output) *output = nullptr;
    if (!enabled.load(std::memory_order_acquire) || !installed || !factory || !output ||
        creatingStrictFilter || (tier != 15 && tier != 20)) return -1;
    struct Scope {
        explicit Scope(std::uint16_t tier) {
            creatingStrictFilter = true;
            wrappedStrictFilter = false;
            creatingStrictTier = tier;
        }
        ~Scope() {
            creatingStrictFilter = false;
            wrappedStrictFilter = false;
            creatingStrictTier = 0;
        }
    } scope(tier);
    const auto result = factory(allocator, allowedSubtypes, nullptr, output);
    if (result != 0 || !wrappedStrictFilter) {
        if (*output) release(*output);
        *output = nullptr;
        return result != 0 ? result : -1;
    }
    return result;
}

} // namespace ammod::quality
