#include "core/decoder.h"

#include "miniaudio.h"

namespace pm {

namespace {

class MiniaudioDecoder : public IDecoder {
public:
    ~MiniaudioDecoder() override {
        if (ready_) ma_decoder_uninit(&decoder_);
    }

    bool Open(const std::filesystem::path& path, uint32_t channels, uint32_t sampleRate, std::string* error) {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, channels, sampleRate);
#ifdef _WIN32
        const ma_result r = ma_decoder_init_file_w(path.c_str(), &cfg, &decoder_);
#else
        const ma_result r = ma_decoder_init_file(path.c_str(), &cfg, &decoder_);
#endif
        if (r != MA_SUCCESS) {
            if (error) *error = ma_result_description(r);
            return false;
        }
        ready_ = true;
        ma_uint64 len = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder_, &len) == MA_SUCCESS) length_ = len;
        return true;
    }

    uint64_t Read(float* out, uint64_t frames) override {
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&decoder_, out, frames, &got);
        return got;
    }

    bool Seek(uint64_t frame) override { return ma_decoder_seek_to_pcm_frame(&decoder_, frame) == MA_SUCCESS; }

    uint64_t LengthFrames() const override { return length_; }

private:
    ma_decoder decoder_{};
    bool ready_ = false;
    uint64_t length_ = 0;
};

}  // namespace

std::unique_ptr<IDecoder> OpenDecoder(const std::filesystem::path& path, uint32_t channels, uint32_t sampleRate,
                                      std::string* error) {
    // Здесь же позже: по расширению .m4a/.aac/.ogg - декодер Media Foundation
    auto d = std::make_unique<MiniaudioDecoder>();
    if (!d->Open(path, channels, sampleRate, error)) return nullptr;
    return d;
}

}  // namespace pm
