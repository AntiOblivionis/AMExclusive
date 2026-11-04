#pragma once

#include <cstdint>
#include <inspectable.h>
#include <winerror.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>

// Read-only ABI prefixes for Apple's private AMP.Services player projection.
//
// The interfaces are ReIID'd between Apple Music releases while their declared
// method order stays identical, so a single binary carries both generations and
// probes the newer one first:
//
//   Apple Music 1.6.4.90  (package 1.1540.23042.0)
//     IPlayer          958DA6F6-2E93-5BA7-B604-28DD57FEB8E0
//     INowPlayingItem  E376ACB7-0569-56C5-8443-A288ACD362EA
//   Apple Music 1.7.0.155 (package 1.1792.39682.0)
//     IPlayer          F807B158-B2E7-5CF7-A512-8B0BE767DF1B
//     INowPlayingItem  2DC5F65C-28F6-58AA-87CF-E4A7DF9B9A4B
//
// IPlayerStatics (34D100F9-0F0A-5980-8F04-7AF57FDB29B0) and IPlayerState
// (BEFB99EB-336F-5199-9FB2-63635BC47319) are unchanged across those releases.
// These declarations let standalone builds observe the existing player without
// redistributing Apple's metadata or generating its full projection.
namespace ammod::apple_private::playback {
namespace abi {

struct __declspec(uuid("34D100F9-0F0A-5980-8F04-7AF57FDB29B0")) IPlayerStatics : ::IInspectable {
    virtual HRESULT __stdcall get_Current(void**) noexcept = 0;
};

// 1.6.4.90 generation.
struct __declspec(uuid("958DA6F6-2E93-5BA7-B604-28DD57FEB8E0")) IPlayer : ::IInspectable {
    virtual HRESULT __stdcall get_NowPlayingItem(void**) noexcept = 0;
    virtual HRESULT __stdcall get_PlayerState(void**) noexcept = 0;
};

// 1.7.0.155 generation; identical vtable prefix, re-generated IID.
struct __declspec(uuid("F807B158-B2E7-5CF7-A512-8B0BE767DF1B")) IPlayerV170 : ::IInspectable {
    virtual HRESULT __stdcall get_NowPlayingItem(void**) noexcept = 0;
    virtual HRESULT __stdcall get_PlayerState(void**) noexcept = 0;
};

struct __declspec(uuid("BEFB99EB-336F-5199-9FB2-63635BC47319")) IPlayerState : ::IInspectable {
    virtual HRESULT __stdcall get_CanPlay(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_CanPause(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_CanStop(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_CanRewind(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_CanFF(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_CanGoBack(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_CanSkipPrevious(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_CanSkipNext(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_PreferMusicControls(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_CanShuffle(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_ShuffleMode(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_CanRepeat(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_RepeatMode(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_TargetPlayRate(double*) noexcept = 0;
    virtual HRESULT __stdcall get_CanSetVolume(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_Volume(double*) noexcept = 0;
    virtual HRESULT __stdcall get_CanMute(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_IsMuted(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_IsPaused(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_IsPlaying(bool*) noexcept = 0;
};

// 1.6.4.90 generation.
struct __declspec(uuid("E376ACB7-0569-56C5-8443-A288ACD362EA")) INowPlayingItem : ::IInspectable {
    virtual HRESULT __stdcall get_Subtitle(void**) noexcept = 0;
    virtual HRESULT __stdcall get_Artist(void**) noexcept = 0;
    virtual HRESULT __stdcall get_Album(void**) noexcept = 0;
    virtual HRESULT __stdcall get_TrackNumber(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_TrackCount(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_Artwork(void**) noexcept = 0;
    virtual HRESULT __stdcall get_PlaybackMediaType(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_ExplicitState(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_VideoRatingString(void**) noexcept = 0;
    virtual HRESULT __stdcall get_RottonTomatoRating(void**) noexcept = 0;
    virtual HRESULT __stdcall get_RottonTomatoScore(void**) noexcept = 0;
    virtual HRESULT __stdcall get_CommonSenseRating(void**) noexcept = 0;
    virtual HRESULT __stdcall get_ISOReleaseDate(void**) noexcept = 0;
    virtual HRESULT __stdcall get_Name(void**) noexcept = 0;
    virtual HRESULT __stdcall get_TVShowName(void**) noexcept = 0;
    virtual HRESULT __stdcall get_Genre(void**) noexcept = 0;
    virtual HRESULT __stdcall get_Description(void**) noexcept = 0;
    virtual HRESULT __stdcall get_Accessibility(void**) noexcept = 0;
    virtual HRESULT __stdcall get_IsPlaceholder(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_IsRadio(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_IsRadioLiveStream(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_Reactions(void**) noexcept = 0;
    virtual HRESULT __stdcall get_ShowCanonicalId(void**) noexcept = 0;
    virtual HRESULT __stdcall get_ScrubberMode(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_CurrentPosition(std::int64_t*) noexcept = 0;
    virtual HRESULT __stdcall get_Duration(std::int64_t*) noexcept = 0;
    virtual HRESULT __stdcall get_LoadTimeIsValid(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_LoadTime(std::int64_t*) noexcept = 0;
    virtual HRESULT __stdcall get_MinLoadTime(std::int64_t*) noexcept = 0;
    virtual HRESULT __stdcall get_CanBePositioned(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_LiveMode(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_CurrentDate(std::uint64_t*) noexcept = 0;
    virtual HRESULT __stdcall get_AudioFormat(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_AudioFormatSampleRate(void**) noexcept = 0;
    virtual HRESULT __stdcall get_AudioFormatBitDepth(void**) noexcept = 0;
    virtual HRESULT __stdcall get_HLSMetadata(void**) noexcept = 0;
    virtual HRESULT __stdcall get_HasVideo(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_VideoPresentationSize(winrt::Windows::Foundation::Size*) noexcept = 0;
    virtual HRESULT __stdcall get_VideoColorCapability(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_PlayQueueItemID(std::uint64_t*) noexcept = 0;
};

// 1.7.0.155 generation; identical vtable prefix, re-generated IID.
struct __declspec(uuid("2DC5F65C-28F6-58AA-87CF-E4A7DF9B9A4B")) INowPlayingItemV170 : ::IInspectable {
    virtual HRESULT __stdcall get_Subtitle(void**) noexcept = 0;
    virtual HRESULT __stdcall get_Artist(void**) noexcept = 0;
    virtual HRESULT __stdcall get_Album(void**) noexcept = 0;
    virtual HRESULT __stdcall get_TrackNumber(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_TrackCount(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_Artwork(void**) noexcept = 0;
    virtual HRESULT __stdcall get_PlaybackMediaType(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_ExplicitState(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_VideoRatingString(void**) noexcept = 0;
    virtual HRESULT __stdcall get_RottonTomatoRating(void**) noexcept = 0;
    virtual HRESULT __stdcall get_RottonTomatoScore(void**) noexcept = 0;
    virtual HRESULT __stdcall get_CommonSenseRating(void**) noexcept = 0;
    virtual HRESULT __stdcall get_ISOReleaseDate(void**) noexcept = 0;
    virtual HRESULT __stdcall get_Name(void**) noexcept = 0;
    virtual HRESULT __stdcall get_TVShowName(void**) noexcept = 0;
    virtual HRESULT __stdcall get_Genre(void**) noexcept = 0;
    virtual HRESULT __stdcall get_Description(void**) noexcept = 0;
    virtual HRESULT __stdcall get_Accessibility(void**) noexcept = 0;
    virtual HRESULT __stdcall get_IsPlaceholder(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_IsRadio(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_IsRadioLiveStream(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_Reactions(void**) noexcept = 0;
    virtual HRESULT __stdcall get_ShowCanonicalId(void**) noexcept = 0;
    virtual HRESULT __stdcall get_ScrubberMode(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_CurrentPosition(std::int64_t*) noexcept = 0;
    virtual HRESULT __stdcall get_Duration(std::int64_t*) noexcept = 0;
    virtual HRESULT __stdcall get_LoadTimeIsValid(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_LoadTime(std::int64_t*) noexcept = 0;
    virtual HRESULT __stdcall get_MinLoadTime(std::int64_t*) noexcept = 0;
    virtual HRESULT __stdcall get_CanBePositioned(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_LiveMode(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_CurrentDate(std::uint64_t*) noexcept = 0;
    virtual HRESULT __stdcall get_AudioFormat(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_AudioFormatSampleRate(void**) noexcept = 0;
    virtual HRESULT __stdcall get_AudioFormatBitDepth(void**) noexcept = 0;
    virtual HRESULT __stdcall get_HLSMetadata(void**) noexcept = 0;
    virtual HRESULT __stdcall get_HasVideo(bool*) noexcept = 0;
    virtual HRESULT __stdcall get_VideoPresentationSize(winrt::Windows::Foundation::Size*) noexcept = 0;
    virtual HRESULT __stdcall get_VideoColorCapability(std::int32_t*) noexcept = 0;
    virtual HRESULT __stdcall get_PlayQueueItemID(std::uint64_t*) noexcept = 0;
};

} // namespace abi

// Which private ABI generation the live AMP.Services.Player object exposes.
enum class PlayerAbi {
    Unknown,
    Legacy, // <= 1.6.4.90 IIDs
    Modern, // >= 1.7.0.155 IIDs
};

namespace detail {

template <typename Interface>
winrt::com_ptr<Interface> QueryInterfaceOrNull(
    winrt::Windows::Foundation::IInspectable const& object) noexcept {
    winrt::com_ptr<Interface> result;
    if (FAILED(object.as(winrt::guid{__uuidof(Interface)}, result.put_void()))) {
        result = nullptr;
    }
    return result;
}

} // namespace detail

inline PlayerAbi DetectPlayerAbi(winrt::Windows::Foundation::IInspectable const& object) noexcept {
    if (detail::QueryInterfaceOrNull<abi::IPlayerV170>(object)) return PlayerAbi::Modern;
    if (detail::QueryInterfaceOrNull<abi::IPlayer>(object)) return PlayerAbi::Legacy;
    return PlayerAbi::Unknown;
}

inline wchar_t const* PlayerAbiName(PlayerAbi abi) noexcept {
    switch (abi) {
    case PlayerAbi::Modern: return L"modern";
    case PlayerAbi::Legacy: return L"legacy";
    case PlayerAbi::Unknown: break;
    }
    return L"unknown";
}

struct ObservableObject : winrt::Windows::Foundation::IInspectable {
    using IInspectable::IInspectable;

    winrt::event_token PropertyChanged(
        winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler) const {
        return as<winrt::Microsoft::UI::Xaml::Data::INotifyPropertyChanged>().PropertyChanged(handler);
    }

    void PropertyChanged(winrt::event_token token) const {
        as<winrt::Microsoft::UI::Xaml::Data::INotifyPropertyChanged>().PropertyChanged(token);
    }
};

struct PlayerState : ObservableObject {
    using ObservableObject::ObservableObject;

    bool IsPlaying() const {
        bool value{};
        winrt::check_hresult(as<abi::IPlayerState>()->get_IsPlaying(&value));
        return value;
    }

    bool IsPaused() const {
        bool value{};
        winrt::check_hresult(as<abi::IPlayerState>()->get_IsPaused(&value));
        return value;
    }
};

struct NowPlayingItem : ObservableObject {
    using ObservableObject::ObservableObject;

    std::uint64_t PlayQueueItemID() const {
        std::uint64_t value{};
        if (auto modern = detail::QueryInterfaceOrNull<abi::INowPlayingItemV170>(*this)) {
            winrt::check_hresult(modern->get_PlayQueueItemID(&value));
        } else if (auto legacy = detail::QueryInterfaceOrNull<abi::INowPlayingItem>(*this)) {
            winrt::check_hresult(legacy->get_PlayQueueItemID(&value));
        } else {
            winrt::throw_hresult(E_NOINTERFACE);
        }
        return value;
    }
};

struct Player : ObservableObject {
    using ObservableObject::ObservableObject;

    static Player Current() {
        auto factory = winrt::get_activation_factory<abi::IPlayerStatics>(L"AMP.Services.Player");
        void* value{};
        winrt::check_hresult(factory->get_Current(&value));
        return {value, winrt::take_ownership_from_abi};
    }

    playback::PlayerState PlayerState() const {
        void* value{};
        if (auto modern = detail::QueryInterfaceOrNull<abi::IPlayerV170>(*this)) {
            winrt::check_hresult(modern->get_PlayerState(&value));
        } else if (auto legacy = detail::QueryInterfaceOrNull<abi::IPlayer>(*this)) {
            winrt::check_hresult(legacy->get_PlayerState(&value));
        } else {
            winrt::throw_hresult(E_NOINTERFACE);
        }
        return {value, winrt::take_ownership_from_abi};
    }

    playback::NowPlayingItem NowPlayingItem() const {
        void* value{};
        if (auto modern = detail::QueryInterfaceOrNull<abi::IPlayerV170>(*this)) {
            winrt::check_hresult(modern->get_NowPlayingItem(&value));
        } else if (auto legacy = detail::QueryInterfaceOrNull<abi::IPlayer>(*this)) {
            winrt::check_hresult(legacy->get_NowPlayingItem(&value));
        } else {
            winrt::throw_hresult(E_NOINTERFACE);
        }
        return {value, winrt::take_ownership_from_abi};
    }
};

} // namespace ammod::apple_private::playback
