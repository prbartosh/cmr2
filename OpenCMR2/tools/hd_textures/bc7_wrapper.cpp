// Multithreaded C ABI wrapper around Richard Geldreich's bc7enc_rdo (bc7enc.h/cpp)
// for OpenCMR2 Stage 10 HD Texture Pipeline (`bc7_dds.py`).
//
// Build on macOS (Apple Silicon / Intel):
//   clang++ -O3 -std=c++17 -shared -fPIC \
//       -I"$HOME/.cache/opencmr2_hd/bc7enc" \
//       "$HOME/.cache/opencmr2_hd/bc7enc/bc7enc.cpp" \
//       bc7_wrapper.cpp \
//       -o "$HOME/.cache/opencmr2_hd/bc7enc/libbc7enc.dylib"
//
// Build on Linux:
//   g++ -O3 -std=c++17 -shared -fPIC -pthread \
//       -I"$HOME/.cache/opencmr2_hd/bc7enc" \
//       "$HOME/.cache/opencmr2_hd/bc7enc/bc7enc.cpp" \
//       bc7_wrapper.cpp \
//       -o "$HOME/.cache/opencmr2_hd/bc7enc/libbc7enc.so"

#include "bc7enc.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

std::once_flag g_bc7InitFlag;

void EnsureBC7Init()
{
    std::call_once(g_bc7InitFlag, []() { bc7enc_compress_block_init(); });
}

} // namespace

// Encodes tightly packed RGBA8 (`width * height * 4` bytes) into tightly packed
// 16-byte BC7 blocks (`((width + 3) / 4) * ((height + 3) / 4) * 16` bytes).
// Non-multiple-of-4 edges (including 2x2 and 1x1 mipmaps) are edge-clamped.
//
// Parameters:
//   uber_level     : 0..4 (BC7ENC_MAX_UBER_LEVEL)
//   max_partitions : 0..64 (BC7ENC_MAX_PARTITIONS; 0 = Mode 6 only)
//   perceptual     : 1 for sRGB/albedo perceptual weights, 0 for linear data (normal/spec maps)
//   num_threads    : <= 0 auto-detects hardware concurrency
extern "C" void bc7_encode_rgba_mt(
    const uint8_t *rgba,
    int width,
    int height,
    uint8_t *out_blocks,
    int uber_level,
    int max_partitions,
    int perceptual,
    int num_threads)
{
    if (!rgba || !out_blocks || width <= 0 || height <= 0)
        return;

    EnsureBC7Init();

    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    if (perceptual)
        bc7enc_compress_block_params_init_perceptual_weights(&params);
    else
        bc7enc_compress_block_params_init_linear_weights(&params);

    params.m_uber_level = static_cast<uint32_t>(std::clamp(uber_level, 0, BC7ENC_MAX_UBER_LEVEL));
    params.m_max_partitions = static_cast<uint32_t>(std::clamp(max_partitions, 0, BC7ENC_MAX_PARTITIONS));

    const int bw = (width + 3) / 4;
    const int bh = (height + 3) / 4;
    const int totalBlocks = bw * bh;

    int threads = (num_threads > 0) ? num_threads : static_cast<int>(std::thread::hardware_concurrency());
    if (threads < 1)
        threads = 1;
    if (totalBlocks < 256)
        threads = 1;
    if (threads > bh)
        threads = bh;

    auto worker = [&](int yStart, int yEnd) {
        uint8_t blockPixels[64];
        for (int by = yStart; by < yEnd; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                for (int py = 0; py < 4; ++py) {
                    const int sy = std::min(height - 1, by * 4 + py);
                    const uint8_t *row = rgba + static_cast<size_t>(sy) * static_cast<size_t>(width) * 4u;
                    for (int px = 0; px < 4; ++px) {
                        const int sx = std::min(width - 1, bx * 4 + px);
                        std::memcpy(blockPixels + (py * 4 + px) * 4, row + static_cast<size_t>(sx) * 4u, 4);
                    }
                }
                uint8_t *dst = out_blocks + (static_cast<size_t>(by) * static_cast<size_t>(bw) + static_cast<size_t>(bx)) * 16u;
                bc7enc_compress_block(dst, blockPixels, &params);
            }
        }
    };

    if (threads <= 1) {
        worker(0, bh);
        return;
    }

    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    const int rowsPerThread = (bh + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        const int y0 = t * rowsPerThread;
        const int y1 = std::min(bh, y0 + rowsPerThread);
        if (y0 < y1)
            pool.emplace_back(worker, y0, y1);
    }
    for (auto &th : pool)
        th.join();
}
