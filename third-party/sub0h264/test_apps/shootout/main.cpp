/** Sub0h264 Shootout — A-vs-B decoder performance comparison
 *
 *  Decodes benchmark fixtures with Sub0h264 and libavc (both linked as
 *  static libraries). Reports comparative fps and ratio.
 *
 *  Build with MinGW (required for libavc source compatibility):
 *    cmake -G "MinGW Makefiles" -B build-shootout -DCMAKE_BUILD_TYPE=Release \
 *          test_apps/shootout
 *    cmake --build build-shootout
 *
 *  Usage:
 *    sub0h264_shootout [--fixtures-dir tests/fixtures]
 *
 *  SPDX-License-Identifier: MIT
 */
#include "decoder.hpp"
#include "annexb.hpp"
#include "decode_timing.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if HAVE_LIBAVC
extern "C" {
#include "ih264_typedefs.h"
#include "ih264d.h"
#include "iv.h"
#include "ivd.h"
}
#endif

using namespace sub0h264;

static std::vector<uint8_t> loadFile(const char* path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> d(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(d.data()), sz);
    return d;
}

static constexpr uint32_t cPasses = 3U;

// ── Sub0h264 benchmark ─────────────────────────────────────────────────

static double benchSub0h264(const std::vector<uint8_t>& data, int32_t& outFrames)
{
    auto dec = std::make_unique<H264Decoder>();
    outFrames = dec->decodeStream(data.data(), static_cast<uint32_t>(data.size()));
    if (outFrames <= 0) return 0.0;

    // Warm-up
    dec = std::make_unique<H264Decoder>();
    dec->decodeStream(data.data(), static_cast<uint32_t>(data.size()));

    std::array<double, cPasses> ms = {};
    for (uint32_t p = 0U; p < cPasses; ++p)
    {
        dec = std::make_unique<H264Decoder>();
        int64_t t0 = sub0h264TimerUs();
        dec->decodeStream(data.data(), static_cast<uint32_t>(data.size()));
        ms[p] = (sub0h264TimerUs() - t0) / 1000.0;
    }
    std::sort(ms.begin(), ms.end());
    double med = ms[cPasses / 2];
    return (med > 0.0) ? (outFrames * 1000.0 / med) : 0.0;
}

// ── libavc benchmark ────────────────────────────────────────────────────

#if HAVE_LIBAVC
static double benchLibavc(const std::vector<uint8_t>& data, int32_t expectedFrames)
{
    // Create decoder — matching avcdec example initialization exactly
    ih264d_create_ip_t createIn = {};
    ih264d_create_op_t createOut = {};
    createIn.s_ivd_create_ip_t.u4_size = sizeof(ih264d_create_ip_t);
    createIn.s_ivd_create_ip_t.e_cmd = IVD_CMD_CREATE;
    createIn.s_ivd_create_ip_t.u4_share_disp_buf = 0;
    createIn.s_ivd_create_ip_t.e_output_format = IV_YUV_420P;
    createIn.s_ivd_create_ip_t.pf_aligned_alloc = [](void* ctx, WORD32 align, WORD32 size) -> void* {
        (void)ctx;
        return _aligned_malloc(static_cast<size_t>(size), static_cast<size_t>(align));
    };
    createIn.s_ivd_create_ip_t.pf_aligned_free = [](void*, void* ptr) {
        _aligned_free(ptr);
    };
    createIn.s_ivd_create_ip_t.pv_mem_ctxt = nullptr;
    createIn.u4_enable_frame_info = 0;
    createIn.u4_keep_threads_active = 0;
    createOut.s_ivd_create_op_t.u4_size = sizeof(ih264d_create_op_t);

    iv_obj_t* codec = nullptr;
    IV_API_CALL_STATUS_T status = ih264d_api_function(
        nullptr, &createIn, &createOut);
    if (status != IV_SUCCESS)
    {
        std::fprintf(stderr, "libavc: CREATE failed (status=%d, error=0x%x)\n",
            status, createOut.s_ivd_create_op_t.u4_error_code);
        return 0.0;
    }
    codec = reinterpret_cast<iv_obj_t*>(createOut.s_ivd_create_op_t.pv_handle);
    // libavc CREATE allocates the handle but doesn't set u4_size/pv_fxns —
    // the caller must initialize them (validated at every subsequent API call).
    codec->u4_size = sizeof(iv_obj_t);
    codec->pv_fxns = reinterpret_cast<void*>(ih264d_api_function);

    // Set single-core decode for fair comparison
    ih264d_ctl_set_num_cores_ip_t coresIn = {};
    ih264d_ctl_set_num_cores_op_t coresOut = {};
    coresIn.e_cmd = IVD_CMD_VIDEO_CTL;
    coresIn.e_sub_cmd = (IVD_CONTROL_API_COMMAND_TYPE_T)IH264D_CMD_CTL_SET_NUM_CORES;
    coresIn.u4_num_cores = 1;
    coresIn.u4_size = sizeof(coresIn);
    coresOut.u4_size = sizeof(coresOut);
    ih264d_api_function(codec, &coresIn, &coresOut);

    // Allocate output buffers
    constexpr uint32_t maxW = 1920, maxH = 1088;
    std::vector<uint8_t> yBuf(maxW * maxH);
    std::vector<uint8_t> uBuf(maxW * maxH / 4);
    std::vector<uint8_t> vBuf(maxW * maxH / 4);
    // QP/block type maps for ih264d extended structs
    constexpr uint32_t mapSize = (maxW * maxH) >> 6; // one per 8x8 block
    std::vector<uint8_t> qpMap(mapSize);
    std::vector<uint8_t> blkTypeMap(mapSize);

    auto decodeAll = [&]() -> int32_t {
        int32_t frames = 0;
        size_t offset = 0;
        while (offset < data.size())
        {
            ih264d_video_decode_ip_t decIn = {};
            ih264d_video_decode_op_t decOut = {};
            decIn.s_ivd_video_decode_ip_t.u4_size = sizeof(ih264d_video_decode_ip_t);
            decIn.s_ivd_video_decode_ip_t.e_cmd = IVD_CMD_VIDEO_DECODE;
            decIn.s_ivd_video_decode_ip_t.pv_stream_buffer = const_cast<uint8_t*>(data.data() + offset);
            decIn.s_ivd_video_decode_ip_t.u4_num_Bytes = static_cast<UWORD32>(data.size() - offset);
            decIn.s_ivd_video_decode_ip_t.s_out_buffer.u4_num_bufs = 3;
            decIn.s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[0] = yBuf.data();
            decIn.s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[1] = uBuf.data();
            decIn.s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[2] = vBuf.data();
            decIn.s_ivd_video_decode_ip_t.s_out_buffer.u4_min_out_buf_size[0] = static_cast<UWORD32>(yBuf.size());
            decIn.s_ivd_video_decode_ip_t.s_out_buffer.u4_min_out_buf_size[1] = static_cast<UWORD32>(uBuf.size());
            decIn.s_ivd_video_decode_ip_t.s_out_buffer.u4_min_out_buf_size[2] = static_cast<UWORD32>(vBuf.size());

            decIn.pu1_8x8_blk_qp_map = qpMap.data();
            decIn.pu1_8x8_blk_type_map = blkTypeMap.data();
            decIn.u4_8x8_blk_qp_map_size = static_cast<UWORD32>(qpMap.size());
            decIn.u4_8x8_blk_type_map_size = static_cast<UWORD32>(blkTypeMap.size());
            decOut.s_ivd_video_decode_op_t.u4_size = sizeof(ih264d_video_decode_op_t);
            IV_API_CALL_STATUS_T st = ih264d_api_function(codec, &decIn, &decOut);

            auto& op = decOut.s_ivd_video_decode_op_t;
            if (op.u4_num_bytes_consumed == 0)
                break;
            offset += op.u4_num_bytes_consumed;
            if (op.u4_output_present)
                ++frames;
        }
        // Flush remaining frames
        for (int flush = 0; flush < 32; ++flush)
        {
            ih264d_video_decode_ip_t flIn = {};
            ih264d_video_decode_op_t flOut = {};
            flIn.s_ivd_video_decode_ip_t.u4_size = sizeof(ih264d_video_decode_ip_t);
            flIn.s_ivd_video_decode_ip_t.e_cmd = IVD_CMD_VIDEO_DECODE;
            flIn.s_ivd_video_decode_ip_t.pv_stream_buffer = nullptr;
            flIn.s_ivd_video_decode_ip_t.u4_num_Bytes = 0;
            flIn.s_ivd_video_decode_ip_t.s_out_buffer.u4_num_bufs = 3;
            flIn.s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[0] = yBuf.data();
            flIn.s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[1] = uBuf.data();
            flIn.s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[2] = vBuf.data();
            flIn.s_ivd_video_decode_ip_t.s_out_buffer.u4_min_out_buf_size[0] = static_cast<UWORD32>(yBuf.size());
            flIn.s_ivd_video_decode_ip_t.s_out_buffer.u4_min_out_buf_size[1] = static_cast<UWORD32>(uBuf.size());
            flIn.s_ivd_video_decode_ip_t.s_out_buffer.u4_min_out_buf_size[2] = static_cast<UWORD32>(vBuf.size());
            flOut.s_ivd_video_decode_op_t.u4_size = sizeof(ih264d_video_decode_op_t);
            ih264d_api_function(codec, &flIn, &flOut);
            if (flOut.s_ivd_video_decode_op_t.u4_output_present) ++frames;
            else break;
        }
        return frames;
    };

    // Validate + warm-up
    int32_t valFrames = decodeAll();
    if (valFrames == 0)
    {
        std::fprintf(stderr, "libavc: decoded 0 frames for validation\n");
        // Delete decoder and return 0
        ih264d_delete_ip_t delIn2 = {};
        ih264d_delete_op_t delOut2 = {};
        delIn2.s_ivd_delete_ip_t.u4_size = sizeof(delIn2);
        delIn2.s_ivd_delete_ip_t.e_cmd = IVD_CMD_DELETE;
        delOut2.s_ivd_delete_op_t.u4_size = sizeof(delOut2);
        ih264d_api_function(codec, &delIn2, &delOut2);
        return 0.0;
    }

    // Reset decoder for measured passes
    std::array<double, cPasses> ms = {};
    for (uint32_t p = 0; p < cPasses; ++p)
    {
        // Reset
        ivd_ctl_reset_ip_t rstIn = {};
        ivd_ctl_reset_op_t rstOut = {};
        rstIn.u4_size = sizeof(rstIn);
        rstIn.e_cmd = IVD_CMD_VIDEO_CTL;
        rstIn.e_sub_cmd = IVD_CMD_CTL_RESET;
        rstOut.u4_size = sizeof(rstOut);
        ih264d_api_function(codec, &rstIn, &rstOut);

        int64_t t0 = sub0h264TimerUs();
        decodeAll();
        ms[p] = (sub0h264TimerUs() - t0) / 1000.0;
    }

    // Delete decoder
    ih264d_delete_ip_t delIn = {};
    ih264d_delete_op_t delOut = {};
    delIn.s_ivd_delete_ip_t.u4_size = sizeof(delIn);
    delIn.s_ivd_delete_ip_t.e_cmd = IVD_CMD_DELETE;
    delOut.s_ivd_delete_op_t.u4_size = sizeof(delOut);
    ih264d_api_function(codec, &delIn, &delOut);

    std::sort(ms.begin(), ms.end());
    double med = ms[cPasses / 2];
    return (med > 0.0) ? (expectedFrames * 1000.0 / med) : 0.0;
}
#endif

// ── Fixture list ────────────────────────────────────────────────────────

struct Fixture { const char* id; const char* filename; };

static constexpr Fixture cFixtures[] = {
    {"Scroll-Base-640",    "bench_scroll_baseline_640x480.h264"},
    {"Scroll-High-640",    "bench_scroll_high_640x480.h264"},
    {"Ball-Base-640",      "bench_ball_baseline_640x480.h264"},
    {"Ball-High-640",      "bench_ball_high_640x480.h264"},
    {"Still-Base-640",     "bench_still_baseline_640x480.h264"},
    {"Still-High-640",     "bench_still_high_640x480.h264"},
    {"Flat-Base-640",      "flat_black_baseline_640x480.h264"},
    {"CAVLC-Base-640",     "baseline_640x480_short.h264"},
    {"Tapo-C110",          "tapo_c110_stream2_high.h264"},
};

int main(int argc, char** argv)
{
    const char* fixturesDir = "tests/fixtures";
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--fixtures-dir") == 0 && i + 1 < argc)
            fixturesDir = argv[++i];

    std::printf("Sub0h264 Shootout\n");
    std::printf("Fixtures: %s\n", fixturesDir);
#if HAVE_LIBAVC
    std::printf("libavc:   linked from source (single-core)\n\n");
#else
    std::printf("libavc:   NOT AVAILABLE (build with HAVE_LIBAVC)\n\n");
#endif

    std::printf("%-25s %12s %12s %8s\n", "Stream", "sub0h264", "libavc", "Ratio");
    std::printf("%s\n", std::string(60, '-').c_str());

    for (const auto& fix : cFixtures)
    {
        std::string path = std::string(fixturesDir) + "/" + fix.filename;
        auto data = loadFile(path.c_str());
        if (data.empty())
        {
            std::printf("%-25s SKIPPED\n", fix.id);
            continue;
        }

        int32_t frames = 0;
        double sub0fps = benchSub0h264(data, frames);

        double libavcFps = 0.0;
#if HAVE_LIBAVC
        libavcFps = benchLibavc(data, frames);
#endif

        double ratio = (libavcFps > 0.0) ? (sub0fps / libavcFps) : 0.0;
        std::printf("%-25s %10.1f fps %10.1f fps %6.2fx\n",
            fix.id, sub0fps, libavcFps, ratio);

        std::fprintf(stderr, "SHOOTOUT_JSON {\"stream\":\"%s\",\"decoder\":\"sub0h264\","
            "\"fps\":%.1f,\"frames\":%d}\n", fix.id, sub0fps, frames);
        if (libavcFps > 0.0)
            std::fprintf(stderr, "SHOOTOUT_JSON {\"stream\":\"%s\",\"decoder\":\"libavc\","
                "\"fps\":%.1f,\"frames\":%d}\n", fix.id, libavcFps, frames);
    }

    return 0;
}
