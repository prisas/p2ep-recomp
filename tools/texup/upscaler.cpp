#include "tools/texup/upscaler.h"

#include "lib/common/file.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}
#pragma clang diagnostic pop

#include <cstring>
#include <memory>

namespace p2::texup {

namespace {

struct GraphFree {
    void operator()(AVFilterGraph* graph) const { avfilter_graph_free(&graph); }
};
struct FrameFree {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
struct InOutFree {
    void operator()(AVFilterInOut* list) const { avfilter_inout_free(&list); }
};

std::string av_error(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

bool fail(std::string* error, const std::string& what, int code = 0) {
    if (error) *error = code ? what + ": " + av_error(code) : what;
    return false;
}

} // namespace

bool run_filters(const std::vector<u8>& bgr0, u32 width, u32 height, const std::string& chain,
                 std::vector<u8>& out, u32& out_width, u32& out_height, std::string* error) {
    if (!width || !height || bgr0.size() < size_t(width) * height * 4) return fail(error, "empty picture");
    std::unique_ptr<AVFilterGraph, GraphFree> graph(avfilter_graph_alloc());
    if (!graph) return fail(error, "out of memory");
    graph->nb_threads = 1; // the program works on one texture per thread already

    AVFilterContext* source = nullptr;
    AVFilterContext* sink = nullptr;
    const std::string args = format("video_size=%ux%u:pix_fmt=bgr0:time_base=1/1:pixel_aspect=1/1", width, height);
    int rc = avfilter_graph_create_filter(&source, avfilter_get_by_name("buffer"), "in", args.c_str(), nullptr, graph.get());
    if (rc < 0) return fail(error, "cannot create the filter input", rc);
    rc = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph.get());
    if (rc < 0) return fail(error, "cannot create the filter output", rc);

    // The chain reads from "in" and writes to "out".
    std::unique_ptr<AVFilterInOut, InOutFree> outputs(avfilter_inout_alloc()), inputs(avfilter_inout_alloc());
    if (!outputs || !inputs) return fail(error, "out of memory");
    outputs->name = av_strdup("in");
    outputs->filter_ctx = source;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = sink;
    AVFilterInOut* in_list = inputs.release();
    AVFilterInOut* out_list = outputs.release();
    rc = avfilter_graph_parse_ptr(graph.get(), chain.c_str(), &in_list, &out_list, nullptr);
    inputs.reset(in_list);
    outputs.reset(out_list);
    if (rc < 0) return fail(error, "bad filter chain '" + chain + "'", rc);
    if ((rc = avfilter_graph_config(graph.get(), nullptr)) < 0) return fail(error, "cannot set up the filters", rc);

    std::unique_ptr<AVFrame, FrameFree> frame(av_frame_alloc());
    if (!frame) return fail(error, "out of memory");
    frame->width = int(width);
    frame->height = int(height);
    frame->format = AV_PIX_FMT_BGR0;
    frame->pts = 0;
    if ((rc = av_frame_get_buffer(frame.get(), 0)) < 0) return fail(error, "cannot allocate a picture", rc);
    for (u32 y = 0; y < height; ++y)
        std::memcpy(frame->data[0] + size_t(y) * size_t(frame->linesize[0]), bgr0.data() + size_t(y) * width * 4, size_t(width) * 4);
    if ((rc = av_buffersrc_add_frame(source, frame.get())) < 0) return fail(error, "cannot feed the filters", rc);
    if ((rc = av_buffersrc_add_frame(source, nullptr)) < 0) return fail(error, "cannot end the input", rc);

    std::unique_ptr<AVFrame, FrameFree> result(av_frame_alloc());
    if (!result) return fail(error, "out of memory");
    if ((rc = av_buffersink_get_frame(sink, result.get())) < 0) return fail(error, "no picture came out of the filters", rc);
    if (result->format != AV_PIX_FMT_BGR0) return fail(error, "the filter chain does not end in bgr0");

    out_width = u32(result->width);
    out_height = u32(result->height);
    out.resize(size_t(out_width) * out_height * 4);
    for (u32 y = 0; y < out_height; ++y)
        std::memcpy(out.data() + size_t(y) * out_width * 4, result->data[0] + size_t(y) * size_t(result->linesize[0]),
                    size_t(out_width) * 4);
    return true;
}

bool upscale(const Rgba& in, const Settings& settings, Rgba& out, std::string* error) {
    if (settings.scale < 2 || settings.scale > 4) return fail(error, "the scale must be 2, 3 or 4");
    const bool transparent = has_transparency(in);
    Rgba prepared = in;
    if (transparent) fill_transparent(prepared);

    std::string chain = format("xbr=n=%u", settings.scale);
    if (settings.sharpen > 0.0f) append_format(chain, ",format=gbrp,cas=strength=%.3f", double(settings.sharpen));
    chain += ",format=bgr0";
    std::vector<u8> color, alpha;
    u32 w = 0, h = 0, aw = 0, ah = 0;
    if (!run_filters(color_bgr0(prepared), in.width, in.height, chain, color, w, h, error)) return false;
    if (transparent) {
        // Alpha is not sharpened: it stays a clean outline.
        if (!run_filters(alpha_bgr0(in), in.width, in.height, format("xbr=n=%u,format=bgr0", settings.scale), alpha, aw, ah, error))
            return false;
        if (aw != w || ah != h) return fail(error, "colour and alpha came out at different sizes");
    }
    out = merge_bgr0(w, h, color, transparent ? &alpha : nullptr);
    return true;
}

} // namespace p2::texup
