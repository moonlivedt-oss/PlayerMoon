// Декодер Media Foundation (только Windows): M4A/AAC, ALAC, а с «Расширениями для веб-мультимедиа»
// (стоят в Windows 10/11 по умолчанию, но их можно удалить) - Ogg Vorbis и Opus.
//
// IMFSourceReader отдаёт float в родной частоте и числе каналов файла; в формат движка переводит
// ma_data_converter из miniaudio (как у MiniaudioDecoder). Перемотка Media Foundation попадает
// на ближайший кадр кодека до нужного места; лишние сэмплы до цели отбрасываются по метке времени.
//
// COM: OpenMfDecoder зовёт поток UI (STA), Read и Seek - аудиопоток miniaudio (MTA). Source Reader
// не привязан к апартаменту, поэтому так можно.
#ifdef _WIN32

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/decoder.h"
#include "miniaudio.h"

namespace pm {

namespace {

constexpr DWORD kAudio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

template <class T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string HrText(const char* what, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s: 0x%08lX", what, static_cast<unsigned long>(hr));
    if (hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr == MF_E_TOPO_CODEC_NOT_FOUND || hr == MF_E_INVALIDMEDIATYPE)
        return std::string(buf) + " (нет декодера: для OGG и Opus нужны «Расширения для веб-мультимедиа» из Microsoft Store)";
    return buf;
}

bool StartMf() {
    static std::once_flag once;
    static bool ok = false;
    // MFShutdown не зовём: Media Foundation живёт до конца процесса
    std::call_once(once, [] { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); });
    return ok;
}

class MfDecoder : public IDecoder {
public:
    ~MfDecoder() override {
        if (converterReady_) ma_data_converter_uninit(&converter_, nullptr);
        Release(reader_);
    }

    bool Open(const std::filesystem::path& path, uint32_t channels, uint32_t sampleRate, std::string* error) {
        outChannels_ = channels;
        outRate_ = sampleRate;
        if (!StartMf()) return Fail(error, "MFStartup", E_FAIL);
        HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader_);
        if (FAILED(hr)) return Fail(error, "MFCreateSourceReaderFromURL", hr);
        reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        hr = reader_->SetStreamSelection(kAudio, TRUE);
        if (FAILED(hr)) return Fail(error, "нет звуковой дорожки", hr);
        IMFMediaType* want = nullptr;
        hr = MFCreateMediaType(&want);
        if (SUCCEEDED(hr)) hr = want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (SUCCEEDED(hr)) hr = reader_->SetCurrentMediaType(kAudio, nullptr, want);
        Release(want);
        if (FAILED(hr)) return Fail(error, "SetCurrentMediaType", hr);
        if (!UpdateFormat()) return Fail(error, "формат дорожки", E_FAIL);

        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(reader_->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                        MF_PD_DURATION, &v)) &&
            v.vt == VT_UI8)
            length_ = v.uhVal.QuadPart * outRate_ / 10000000ull;
        PropVariantClear(&v);
        return true;
    }

    uint64_t Read(float* out, uint64_t frames) override {
        uint64_t done = 0;
        while (done < frames) {
            if (pendingAt_ >= pendingFrames_) {
                if (eof_ || !Fill()) break;
                continue;
            }
            ma_uint64 inCount = pendingFrames_ - pendingAt_;
            ma_uint64 outCount = frames - done;
            if (ma_data_converter_process_pcm_frames(&converter_, pending_.data() + pendingAt_ * inChannels_, &inCount,
                                                     out + done * outChannels_, &outCount) != MA_SUCCESS)
                break;
            pendingAt_ += inCount;
            done += outCount;
            if (inCount == 0 && outCount == 0) break;  // защита от зацикливания
        }
        return done;
    }

    bool Seek(uint64_t frame) override {
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_I8;
        v.hVal.QuadPart = static_cast<LONGLONG>(frame * 10000000ull / outRate_);
        const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, v);
        PropVariantClear(&v);
        if (FAILED(hr)) return false;
        pendingFrames_ = pendingAt_ = 0;
        eof_ = false;
        seekTarget_ = static_cast<int64_t>(frame * inRate_ / outRate_);
        return InitConverter();  // сброс хвоста ресэмплера
    }

    uint64_t LengthFrames() const override { return length_; }

private:
    bool Fail(std::string* error, const char* what, HRESULT hr) {
        if (error) *error = HrText(what, hr);
        return false;
    }

    // Частота и каналы текущего типа (бывает, меняются посреди файла)
    bool UpdateFormat() {
        IMFMediaType* type = nullptr;
        if (FAILED(reader_->GetCurrentMediaType(kAudio, &type))) return false;
        UINT32 ch = 0, rate = 0;
        type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        Release(type);
        if (ch == 0 || rate == 0) return false;
        inChannels_ = ch;
        inRate_ = rate;
        return InitConverter();
    }

    bool InitConverter() {
        if (converterReady_) ma_data_converter_uninit(&converter_, nullptr);
        converterReady_ = false;
        ma_data_converter_config cfg =
            ma_data_converter_config_init(ma_format_f32, ma_format_f32, inChannels_, outChannels_, inRate_, outRate_);
        cfg.resampling.algorithm = ma_resample_algorithm_linear;
        converterReady_ = ma_data_converter_init(&cfg, nullptr, &converter_) == MA_SUCCESS;
        return converterReady_;
    }

    // Следующий сэмпл Media Foundation -> pending_. false - конец или ошибка.
    bool Fill() {
        for (;;) {
            DWORD flags = 0;
            LONGLONG time = 0;
            IMFSample* sample = nullptr;
            const HRESULT hr = reader_->ReadSample(kAudio, 0, nullptr, &flags, &time, &sample);
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) {
                Release(sample);
                eof_ = true;
                return false;
            }
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && !UpdateFormat()) {
                Release(sample);
                eof_ = true;
                return false;
            }
            if (!sample) continue;  // бывает пустой ответ (разрыв потока)
            IMFMediaBuffer* buffer = nullptr;
            BYTE* data = nullptr;
            DWORD bytes = 0;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) &&
                SUCCEEDED(buffer->Lock(&data, nullptr, &bytes))) {
                const uint64_t frames = bytes / (sizeof(float) * inChannels_);
                // После перемотки: сэмплы до цели выбрасываем
                uint64_t skip = 0;
                if (seekTarget_ >= 0) {
                    const int64_t start = time * static_cast<int64_t>(inRate_) / 10000000;
                    skip = static_cast<uint64_t>(std::clamp<int64_t>(seekTarget_ - start, 0, int64_t(frames)));
                    if (skip < frames) seekTarget_ = -1;
                }
                pendingFrames_ = frames - skip;
                pendingAt_ = 0;
                const size_t floats = static_cast<size_t>(pendingFrames_ * inChannels_);
                if (pending_.size() < floats) pending_.resize(floats);
                std::memcpy(pending_.data(), reinterpret_cast<const float*>(data) + skip * inChannels_,
                            floats * sizeof(float));
                buffer->Unlock();
            }
            Release(buffer);
            Release(sample);
            if (pendingFrames_ > 0) return true;
        }
    }

    IMFSourceReader* reader_ = nullptr;
    ma_data_converter converter_{};
    bool converterReady_ = false;
    uint32_t inChannels_ = 0, inRate_ = 0, outChannels_ = 0, outRate_ = 0;
    std::vector<float> pending_;  // декодированные кадры в родном формате
    uint64_t pendingFrames_ = 0, pendingAt_ = 0;
    int64_t seekTarget_ = -1;  // кадр (в родной частоте), до которого отбрасывать после перемотки
    uint64_t length_ = 0;
    bool eof_ = false;
};

}  // namespace

std::unique_ptr<IDecoder> OpenMfDecoder(const std::filesystem::path& path, uint32_t channels, uint32_t sampleRate,
                                        std::string* error) {
    auto d = std::make_unique<MfDecoder>();
    if (!d->Open(path, channels, sampleRate, error)) return nullptr;
    return d;
}

}  // namespace pm

#endif  // _WIN32
