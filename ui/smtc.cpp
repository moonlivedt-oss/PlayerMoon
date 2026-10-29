#include "ui/smtc.h"

#include <inspectable.h>
#include <roapi.h>
#include <shcore.h>
#include <shlwapi.h>
#include <systemmediatransportcontrolsinterop.h>
#include <winstring.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "core/text.h"

// ---------- Объявления WinRT ABI (порядок методов - как в Windows SDK, windows.media.idl) ----------
// IID параметризованных делегатов посчитаны по правилам WinRT (SHA-1 подписи pinterface); IID
// делегата ButtonPressed сверен с заголовками Wine, тем же способом посчитан и делегат перемотки.

// Не в безымянном пространстве: иначе GCC решает, что у интерфейсов без реализаций в этом файле
// не может быть объектов, и «девиртуализирует» вызовы в мусор (ложное -Wuninitialized).
namespace pm_winrt {

struct TimeSpan {
    INT64 Duration;  // единицы по 100 нс
};

enum PlaybackStatus : int { kClosed = 0, kChanging = 1, kStopped = 2, kPlaying = 3, kPaused = 4 };

// ISystemMediaTransportControlsInterop: интерфейс есть в заголовках mingw, а IID в библиотеках - нет
const IID kIidInterop = {0xddb0472d, 0xc911, 0x4a1f, {0x86, 0xd9, 0xdc, 0x3d, 0x71, 0xa9, 0x5f, 0x5a}};

// {99fa3ff4-1742-42a6-902e-087d41f965ec}
const IID kIidControls = {0x99fa3ff4, 0x1742, 0x42a6, {0x90, 0x2e, 0x08, 0x7d, 0x41, 0xf9, 0x65, 0xec}};
struct ISmtcControls : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(int* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PlaybackStatus(int value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DisplayUpdater(IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SoundLevel(int* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsPlayEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsStopEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsStopEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPauseEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsPauseEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRecordEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsRecordEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsFastForwardEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsFastForwardEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRewindEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsRewindEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPreviousEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsPreviousEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsNextEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsNextEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelUpEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsChannelUpEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelDownEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsChannelDownEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ButtonPressed(IUnknown* handler, INT64* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ButtonPressed(INT64 token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_PropertyChanged(IUnknown* handler, INT64* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_PropertyChanged(INT64 token) = 0;
};

// {ea98d2f6-7f3c-4af2-a586-72889808efb1}
const IID kIidControls2 = {0xea98d2f6, 0x7f3c, 0x4af2, {0xa5, 0x86, 0x72, 0x88, 0x98, 0x08, 0xef, 0xb1}};
struct ISmtcControls2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_AutoRepeatMode(int* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AutoRepeatMode(int value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ShuffleEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ShuffleEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackRate(DOUBLE* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PlaybackRate(DOUBLE value) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateTimelineProperties(IInspectable* timeline) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_PlaybackPositionChangeRequested(IUnknown* handler, INT64* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_PlaybackPositionChangeRequested(INT64 token) = 0;
    // Дальше (скорость, перемешивание, повтор) не используются
};

// {8abbc53e-fa55-4ecf-ad8e-c984e5dd1550}
const IID kIidDisplayUpdater = {0x8abbc53e, 0xfa55, 0x4ecf, {0xad, 0x8e, 0xc9, 0x84, 0xe5, 0xdd, 0x15, 0x50}};
struct ISmtcDisplayUpdater : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Type(int* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Type(int value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AppMediaId(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AppMediaId(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Thumbnail(IInspectable* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MusicProperties(IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_VideoProperties(IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ImageProperties(IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyFromFileAsync(int type, IInspectable* file, IInspectable** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAll() = 0;
    virtual HRESULT STDMETHODCALLTYPE Update() = 0;
};
constexpr int kMediaTypeMusic = 1;

// {6bbf0c59-d0a0-4d26-92a0-f978e1d18e7b}
const IID kIidMusicProperties = {0x6bbf0c59, 0xd0a0, 0x4d26, {0x92, 0xa0, 0xf9, 0x78, 0xe1, 0xd1, 0x8e, 0x7b}};
struct IMusicProperties : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Title(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AlbumArtist(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Artist(HSTRING value) = 0;
};

// {00368462-97d3-44b9-b00f-008afcefaf18}
const IID kIidMusicProperties2 = {0x00368462, 0x97d3, 0x44b9, {0xb0, 0x0f, 0x00, 0x8a, 0xfc, 0xef, 0xaf, 0x18}};
struct IMusicProperties2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTitle(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AlbumTitle(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TrackNumber(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_TrackNumber(UINT32 value) = 0;
};

// {5125316a-c3a2-475b-8507-93534dc88f15}
const IID kIidTimeline = {0x5125316a, 0xc3a2, 0x475b, {0x85, 0x07, 0x93, 0x53, 0x4d, 0xc8, 0x8f, 0x15}};
struct ITimeline : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_StartTime(TimeSpan* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_StartTime(TimeSpan value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_EndTime(TimeSpan* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_EndTime(TimeSpan value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MinSeekTime(TimeSpan* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MinSeekTime(TimeSpan value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxSeekTime(TimeSpan* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MaxSeekTime(TimeSpan value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Position(TimeSpan* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Position(TimeSpan value) = 0;
};

// {b7f47116-a56f-4dc8-9e11-92031f4a87c2}
const IID kIidButtonArgs = {0xb7f47116, 0xa56f, 0x4dc8, {0x9e, 0x11, 0x92, 0x03, 0x1f, 0x4a, 0x87, 0xc2}};
struct IButtonArgs : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Button(int* value) = 0;
};

// {b4493f88-eb28-4961-9c14-335e44f3e125}
const IID kIidPositionArgs = {0xb4493f88, 0xeb28, 0x4961, {0x9c, 0x14, 0x33, 0x5e, 0x44, 0xf3, 0xe1, 0x25}};
struct IPositionArgs : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_RequestedPlaybackPosition(TimeSpan* value) = 0;
};

// TypedEventHandler<SystemMediaTransportControls, ButtonPressedEventArgs>
const IID kIidButtonHandler = {0x0557e996, 0x7b23, 0x5bae, {0xaa, 0x81, 0xea, 0x0d, 0x67, 0x11, 0x43, 0xa4}};
// TypedEventHandler<SystemMediaTransportControls, PlaybackPositionChangeRequestedEventArgs>
const IID kIidPositionHandler = {0x44e34f15, 0xbdc0, 0x50a7, {0xac, 0xe4, 0x39, 0xe9, 0x1f, 0xb7, 0x53, 0xf1}};

// {857309dc-3fbf-4e7d-986f-ef3b1a07a964}
const IID kIidStreamRefStatics = {0x857309dc, 0x3fbf, 0x4e7d, {0x98, 0x6f, 0xef, 0x3b, 0x1a, 0x07, 0xa9, 0x64}};
struct IStreamRefStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateFromFile(IInspectable* file, IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateFromUri(IInspectable* uri, IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateFromStream(IUnknown* stream, IInspectable** value) = 0;
};
// IRandomAccessStream {905a0fe1-bc53-11df-8c49-001e4fc686da}
const IID kIidRandomAccessStream = {0x905a0fe1, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
// IAgileObject: делегат можно звать из любого потока
const IID kIidAgile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

// Указатели в smtc.h хранятся как IUnknown* (типы объявлены только здесь); база - по нулевому смещению
ISmtcControls* Controls(IUnknown* p) { return reinterpret_cast<ISmtcControls*>(p); }
ISmtcControls2* Controls2(IUnknown* p) { return reinterpret_cast<ISmtcControls2*>(p); }

}  // namespace pm_winrt

namespace ui {

namespace {

using namespace pm_winrt;

template <class T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// HSTRING на время вызова
class HStr {
public:
    explicit HStr(const wchar_t* s) { WindowsCreateString(s, static_cast<UINT32>(wcslen(s)), &h_); }
    explicit HStr(const std::string& utf8) {
        const std::wstring w = ::text::Utf8ToWide(utf8);
        WindowsCreateString(w.c_str(), static_cast<UINT32>(w.size()), &h_);
    }
    ~HStr() {
        if (h_) WindowsDeleteString(h_);
    }
    HStr(const HStr&) = delete;
    HStr& operator=(const HStr&) = delete;
    HSTRING get() const { return h_; }

private:
    HSTRING h_ = nullptr;
};

// Делегат TypedEventHandler<SMTC, Args>: читает аргументы и шлёт окну сообщение.
// Методы делегата WinRT: IUnknown + Invoke(sender, args).
class Handler final : public IUnknown {
public:
    using Fn = void (*)(IInspectable* args, HWND hwnd, UINT message);
    Handler(const IID& iid, Fn fn, HWND hwnd, UINT message) : iid_(iid), fn_(fn), hwnd_(hwnd), message_(message) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iid_) || IsEqualIID(riid, kIidAgile)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    // Слот vtable сразу после IUnknown, как у делегатов WinRT
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable* /*sender*/, IInspectable* args) {
        if (args) fn_(args, hwnd_, message_);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    IID iid_;
    Fn fn_;
    HWND hwnd_;
    UINT message_;
};

void OnButton(IInspectable* args, HWND hwnd, UINT message) {
    IButtonArgs* a = nullptr;
    if (FAILED(args->QueryInterface(kIidButtonArgs, reinterpret_cast<void**>(&a)))) return;
    int button = -1;
    if (SUCCEEDED(a->get_Button(&button)) && button >= 0)
        PostMessageW(hwnd, message, static_cast<WPARAM>(button), 0);
    a->Release();
}

void OnSeek(IInspectable* args, HWND hwnd, UINT message) {
    IPositionArgs* a = nullptr;
    if (FAILED(args->QueryInterface(kIidPositionArgs, reinterpret_cast<void**>(&a)))) return;
    TimeSpan t{};
    if (SUCCEEDED(a->get_RequestedPlaybackPosition(&t)) && t.Duration >= 0)
        PostMessageW(hwnd, message, Smtc::kSeek, static_cast<LPARAM>(t.Duration / 10000));
    a->Release();
}

ISmtcDisplayUpdater* GetUpdater(IUnknown* controls) {
    IInspectable* u = nullptr;
    if (FAILED(Controls(controls)->get_DisplayUpdater(&u)) || !u) return nullptr;
    ISmtcDisplayUpdater* updater = nullptr;
    u->QueryInterface(kIidDisplayUpdater, reinterpret_cast<void**>(&updater));
    u->Release();
    return updater;
}

// Обложка из байтов: IStream в памяти -> IRandomAccessStream -> RandomAccessStreamReference
IInspectable* MakeThumbnail(const std::vector<uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() > 16u << 20) return nullptr;
    IStream* mem = SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size()));
    if (!mem) return nullptr;
    IUnknown* ras = nullptr;
    IStreamRefStatics* statics = nullptr;
    IInspectable* ref = nullptr;
    if (SUCCEEDED(CreateRandomAccessStreamOverStream(mem, BSOS_DEFAULT, kIidRandomAccessStream,
                                                     reinterpret_cast<void**>(&ras))) &&
        SUCCEEDED(RoGetActivationFactory(HStr(L"Windows.Storage.Streams.RandomAccessStreamReference").get(),
                                         kIidStreamRefStatics, reinterpret_cast<void**>(&statics))))
        statics->CreateFromStream(ras, &ref);
    Release(statics);
    Release(ras);
    mem->Release();
    return ref;
}

}  // namespace

void Smtc::Attach(HWND hwnd, UINT message) {
    Detach();
    hwnd_ = hwnd;
    message_ = message;
    ISystemMediaTransportControlsInterop* interop = nullptr;
    if (FAILED(RoGetActivationFactory(HStr(L"Windows.Media.SystemMediaTransportControls").get(),
                                      kIidInterop, reinterpret_cast<void**>(&interop))))
        return;
    ISmtcControls* c = nullptr;
    const HRESULT hr = interop->GetForWindow(hwnd, kIidControls, reinterpret_cast<void**>(&c));
    interop->Release();
    if (FAILED(hr) || !c) return;
    controls_ = c;
    c->put_IsPlayEnabled(TRUE);
    c->put_IsPauseEnabled(TRUE);
    c->put_IsStopEnabled(TRUE);
    c->put_IsNextEnabled(TRUE);
    c->put_IsPreviousEnabled(TRUE);
    c->put_IsEnabled(FALSE);  // включится с первым треком
    auto* buttons = new Handler(kIidButtonHandler, &OnButton, hwnd, message);
    if (SUCCEEDED(c->add_ButtonPressed(buttons, &buttonToken_))) buttonHandler_ = buttons;
    else buttons->Release();
    // Позиция и перемотка - Windows 10 1607+
    if (SUCCEEDED(c->QueryInterface(kIidControls2, reinterpret_cast<void**>(&controls2_)))) {
        auto* seek = new Handler(kIidPositionHandler, &OnSeek, hwnd, message);
        if (SUCCEEDED(Controls2(controls2_)->add_PlaybackPositionChangeRequested(seek, &seekToken_)))
            seekHandler_ = seek;
        else seek->Release();
    }
}

void Smtc::Detach() {
    if (controls2_ && seekHandler_) Controls2(controls2_)->remove_PlaybackPositionChangeRequested(seekToken_);
    if (controls_ && buttonHandler_) Controls(controls_)->remove_ButtonPressed(buttonToken_);
    if (controls_) {
        if (ISmtcDisplayUpdater* u = GetUpdater(controls_)) {
            u->ClearAll();
            u->Release();
        }
        Controls(controls_)->put_IsEnabled(FALSE);
    }
    Release(seekHandler_);
    Release(buttonHandler_);
    Release(controls2_);
    Release(controls_);
    status_ = -1;
    hasTrack_ = false;
    duration_ = -1;
}

void Smtc::SetTrack(const std::string& title, const std::string& artist, const std::string& album, int trackNumber,
                    const std::vector<uint8_t>& cover) {
    if (!controls_) return;
    ISmtcDisplayUpdater* u = GetUpdater(controls_);
    if (!u) return;
    u->put_Type(kMediaTypeMusic);
    IInspectable* props = nullptr;
    if (SUCCEEDED(u->get_MusicProperties(&props)) && props) {
        IMusicProperties* m = nullptr;
        if (SUCCEEDED(props->QueryInterface(kIidMusicProperties, reinterpret_cast<void**>(&m)))) {
            m->put_Title(HStr(title).get());
            m->put_Artist(HStr(artist).get());
            m->Release();
        }
        IMusicProperties2* m2 = nullptr;
        if (SUCCEEDED(props->QueryInterface(kIidMusicProperties2, reinterpret_cast<void**>(&m2)))) {
            m2->put_AlbumTitle(HStr(album).get());
            m2->put_TrackNumber(static_cast<UINT32>(std::max(0, trackNumber)));
            m2->Release();
        }
        props->Release();
    }
    IInspectable* thumb = MakeThumbnail(cover);
    u->put_Thumbnail(thumb);  // nullptr - убрать прошлую обложку
    Release(thumb);
    u->Update();
    u->Release();
    duration_ = -1;  // позицию отправить заново
}

void Smtc::ClearTrack() {
    if (!controls_) return;
    if (ISmtcDisplayUpdater* u = GetUpdater(controls_)) {
        u->ClearAll();
        u->Update();
        u->Release();
    }
}

void Smtc::PushTimeline(double position, double duration) {
    if (!controls2_) return;
    IInspectable* obj = nullptr;
    if (FAILED(RoActivateInstance(HStr(L"Windows.Media.SystemMediaTransportControlsTimelineProperties").get(), &obj)) ||
        !obj)
        return;
    ITimeline* t = nullptr;
    if (SUCCEEDED(obj->QueryInterface(kIidTimeline, reinterpret_cast<void**>(&t)))) {
        auto ticks = [](double s) { return TimeSpan{static_cast<INT64>(std::max(0.0, s) * 1e7)}; };
        t->put_StartTime(ticks(0));
        t->put_EndTime(ticks(duration));
        t->put_MinSeekTime(ticks(0));
        t->put_MaxSeekTime(ticks(duration));
        t->put_Position(ticks(std::min(position, duration)));
        Controls2(controls2_)->UpdateTimelineProperties(obj);
        t->Release();
    }
    obj->Release();
}

void Smtc::Update(bool hasTrack, bool playing, double position, double duration) {
    if (!controls_) return;
    auto* c = Controls(controls_);
    if (hasTrack != hasTrack_) {
        c->put_IsEnabled(hasTrack ? TRUE : FALSE);
        hasTrack_ = hasTrack;
    }
    const int status = !hasTrack ? kStopped : playing ? kPlaying : kPaused;
    const bool statusChanged = status != status_;
    if (statusChanged) {
        c->put_PlaybackStatus(status);
        status_ = status;
    }
    if (!hasTrack) return;
    // Позицию отправляем при смене трека или состояния и после перемотки: на ходу Windows сама
    // двигает ползунок. Перемотка - позиция ушла от ожидаемой больше чем на секунду.
    const ULONGLONG now = GetTickCount64();
    const double expected = position_ + (playing ? double(now - positionAt_) / 1000.0 : 0.0);
    if (statusChanged || std::abs(duration - duration_) > 0.01 || std::abs(position - expected) > 1.0) {
        PushTimeline(position, duration);
        duration_ = duration;
    }
    position_ = position;
    positionAt_ = now;
}

}  // namespace ui
