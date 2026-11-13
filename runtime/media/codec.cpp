#include "runtime/media/codec.h"

#include "lib/common/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

#include <algorithm>

namespace p2::media {

// --- audio ----------------------------------------------------------------------------------------------------

struct AudioDecoder::Impl {
    AVCodecContext* ctx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    std::vector<u8> padded;

    ~Impl() { close(); }
    void close() {
        if (ctx) avcodec_free_context(&ctx);
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
    }
};

AudioDecoder::AudioDecoder() : impl_(std::make_unique<Impl>()) {}
AudioDecoder::~AudioDecoder() = default;
AudioDecoder::AudioDecoder(AudioDecoder&&) noexcept = default;
AudioDecoder& AudioDecoder::operator=(AudioDecoder&&) noexcept = default;

bool AudioDecoder::is_open() const { return impl_->ctx != nullptr; }

bool AudioDecoder::open(bool plus, u32 channels, u32 sample_rate, u32 block_align, const u8* extradata, u32 extradata_size) {
    impl_->close();
    const AVCodec* codec = avcodec_find_decoder(plus ? AV_CODEC_ID_ATRAC3P : AV_CODEC_ID_ATRAC3);
    if (!codec) {
        LOG_ERROR("media", "the %s decoder is missing from this FFmpeg build", plus ? "ATRAC3plus" : "ATRAC3");
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    av_channel_layout_default(&ctx->ch_layout, int(channels));
    ctx->sample_rate = int(sample_rate);
    ctx->block_align = int(block_align);
    if (!plus && extradata && extradata_size) {
        ctx->extradata = static_cast<u8*>(av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(ctx->extradata, extradata, extradata_size);
        ctx->extradata_size = int(extradata_size);
    }
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        LOG_ERROR("media", "cannot open the audio decoder (%u channels, %u Hz, %u-byte frames)", channels, sample_rate, block_align);
        return false;
    }
    impl_->ctx = ctx;
    impl_->packet = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    return true;
}

void AudioDecoder::reset() {
    if (impl_->ctx) avcodec_flush_buffers(impl_->ctx);
}

int AudioDecoder::decode(const u8* data, u32 size, std::vector<s16>& out) {
    if (!impl_->ctx) return -1;
    impl_->padded.assign(size_t(size) + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::memcpy(impl_->padded.data(), data, size);
    impl_->packet->data = impl_->padded.data();
    impl_->packet->size = int(size);
    if (avcodec_send_packet(impl_->ctx, impl_->packet) < 0) return -1;

    int produced = 0;
    while (avcodec_receive_frame(impl_->ctx, impl_->frame) == 0) {
        const AVFrame* f = impl_->frame;
        const int n = f->nb_samples, channels = f->ch_layout.nb_channels;
        const size_t at = out.size();
        out.resize(at + size_t(n) * 2);
        auto to_s16 = [](float v) { return s16(std::clamp(int(v * 32768.0f), -32768, 32767)); };
        if (f->format == AV_SAMPLE_FMT_FLTP) {
            const float* left = reinterpret_cast<const float*>(f->extended_data[0]);
            const float* right = channels > 1 ? reinterpret_cast<const float*>(f->extended_data[1]) : left;
            for (int i = 0; i < n; ++i) {
                out[at + size_t(i) * 2] = to_s16(left[i]);
                out[at + size_t(i) * 2 + 1] = to_s16(right[i]);
            }
        } else if (f->format == AV_SAMPLE_FMT_S16P) {
            const s16* left = reinterpret_cast<const s16*>(f->extended_data[0]);
            const s16* right = channels > 1 ? reinterpret_cast<const s16*>(f->extended_data[1]) : left;
            for (int i = 0; i < n; ++i) {
                out[at + size_t(i) * 2] = left[i];
                out[at + size_t(i) * 2 + 1] = right[i];
            }
        } else {
            std::fill(out.begin() + ptrdiff_t(at), out.end(), s16(0));
        }
        produced += n;
        av_frame_unref(impl_->frame);
    }
    return produced;
}

// --- video ----------------------------------------------------------------------------------------------------

struct VideoDecoder::Impl {
    AVCodecContext* ctx = nullptr;
    AVCodecParserContext* parser = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* sws = nullptr;
    std::vector<u8> buffer; // unparsed elementary stream, with decoder padding at the end
    size_t read = 0;
    bool eos = false, flushed = false, have_frame = false;

    ~Impl() { close(); }
    void close() {
        if (parser) av_parser_close(parser);
        parser = nullptr;
        if (ctx) avcodec_free_context(&ctx);
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        if (sws) sws_freeContext(sws);
        sws = nullptr;
    }
};

VideoDecoder::VideoDecoder() : impl_(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder() = default;

bool VideoDecoder::open() {
    impl_->close();
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        LOG_ERROR("media", "the H.264 decoder is missing from this FFmpeg build");
        return false;
    }
    impl_->ctx = avcodec_alloc_context3(codec);
    impl_->ctx->thread_count = 1; // frames must come out as soon as their data is in
    if (avcodec_open2(impl_->ctx, codec, nullptr) < 0) {
        avcodec_free_context(&impl_->ctx);
        return false;
    }
    impl_->parser = av_parser_init(AV_CODEC_ID_H264);
    impl_->packet = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    impl_->buffer.clear();
    impl_->read = 0;
    impl_->eos = impl_->flushed = impl_->have_frame = false;
    return impl_->parser != nullptr;
}

void VideoDecoder::reset() {
    if (impl_->ctx) open();
}

void VideoDecoder::push(const u8* data, size_t size) {
    Impl& s = *impl_;
    if (s.read > (1u << 20)) { // drop what the parser has already consumed
        s.buffer.erase(s.buffer.begin(), s.buffer.begin() + ptrdiff_t(s.read));
        s.read = 0;
    }
    s.buffer.insert(s.buffer.end(), data, data + size);
}

size_t VideoDecoder::pending_bytes() const { return impl_->buffer.size() - impl_->read; }
void VideoDecoder::end_of_stream() { impl_->eos = true; }
bool VideoDecoder::has_frame() const { return impl_->have_frame; }
u32 VideoDecoder::width() const { return impl_->have_frame ? u32(impl_->frame->width) : 0; }
u32 VideoDecoder::height() const { return impl_->have_frame ? u32(impl_->frame->height) : 0; }

bool VideoDecoder::next_frame() {
    Impl& s = *impl_;
    if (!s.ctx) return false;
    while (true) {
        int got = avcodec_receive_frame(s.ctx, s.frame);
        if (got == 0) {
            s.have_frame = true;
            return true;
        }
        if (got != AVERROR(EAGAIN)) return false; // end of stream or a decoding error

        // The decoder wants more input: cut the next access unit out of the byte stream.
        bool sent = false;
        while (!sent) {
            const size_t remaining = s.buffer.size() - s.read;
            if (remaining == 0 && !s.eos) return false;
            // The parser reads a little past the end; keep padding behind the data.
            s.buffer.resize(s.buffer.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
            u8* out = nullptr;
            int out_size = 0;
            int used = av_parser_parse2(s.parser, s.ctx, &out, &out_size, s.buffer.data() + s.read, int(remaining),
                                        AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            s.buffer.resize(s.buffer.size() - AV_INPUT_BUFFER_PADDING_SIZE);
            if (used < 0) return false;
            s.read += size_t(used);
            if (out_size > 0) {
                s.packet->data = out;
                s.packet->size = out_size;
                avcodec_send_packet(s.ctx, s.packet);
                sent = true;
            } else if (remaining == 0) { // end of stream and the parser is drained
                if (s.flushed) return false;
                avcodec_send_packet(s.ctx, nullptr);
                s.flushed = true;
                sent = true;
            } else if (used == 0) {
                return false;
            }
        }
    }
}

void VideoDecoder::convert_rgba(u8* dst, u32 stride, u32 w, u32 h) {
    Impl& s = *impl_;
    if (!s.have_frame) return;
    const AVFrame* f = s.frame;
    const int cw = int(std::min<u32>(w, u32(f->width))), ch = int(std::min<u32>(h, u32(f->height)));
    s.sws = sws_getCachedContext(s.sws, cw, ch, AVPixelFormat(f->format), cw, ch, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                 nullptr, nullptr);
    if (!s.sws) return;
    u8* planes[4] = {dst, nullptr, nullptr, nullptr};
    int strides[4] = {int(stride), 0, 0, 0};
    sws_scale(s.sws, f->data, f->linesize, 0, ch, planes, strides);
}

} // namespace p2::media
