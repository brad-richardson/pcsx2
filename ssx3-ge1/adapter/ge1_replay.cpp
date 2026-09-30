#include "ge1_gs.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#endif

static uint32_t u32at(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static uint64_t u64at(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
static uint64_t hash64(const uint8_t* p, size_t n)
{
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static bool listed_tick(uint64_t tick)
{
    const char* list = std::getenv("GE1_REPLAY_PPM_TICKS");
    if (!list) return false;
    while (*list) {
        char* end = nullptr;
        const unsigned long long value = std::strtoull(list, &end, 10);
        if (end == list) return false;
        if (value == tick) return true;
        list = *end == ',' ? end + 1 : end;
        if (*end != ',') break;
    }
    return false;
}

#ifdef __ANDROID__
// UR1: GE1_REPLAY_EXPORT=WxH exports every frame into a ring of AHBs of that size,
// as the runtime's GE1 present does (one export in flight; its fence is waited
// before the next export, like queuePendingAhb). Off by default.
struct ExportRing
{
    AHardwareBuffer* bufs[4] = {};
    uint32_t w = 0, h = 0, next = 0;
    uint64_t pending = 0, exports = 0, failures = 0;
    bool init()
    {
        const char* s = std::getenv("GE1_REPLAY_EXPORT");
        if (!s || std::sscanf(s, "%ux%u", &w, &h) != 2 || !w || !h)
            return false;
        for (AHardwareBuffer*& b : bufs) {
            AHardwareBuffer_Desc d = {};
            d.width = w; d.height = h; d.layers = 1;
            d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
            d.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                      AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
            if (AHardwareBuffer_allocate(&d, &b) != 0) { std::cerr << "UR1: AHB alloc failed\n"; return false; }
        }
        std::cerr << "UR1: replay export " << w << 'x' << h << " every frame\n";
        return true;
    }
    void frame(uint64_t tick, bool named)
    {
        if (pending) ge1_gs_wait_export(pending);
        pending = 0;
        uint64_t fence = 0;
        AHardwareBuffer* const buf = bufs[next];
        if (ge1_gs_export_ahb(buf, w, h, &fence) == 1) { pending = fence; exports++; }
        else failures++;
        next = (next + 1) % 4;
        // UR2: GE1_REPLAY_AHB_PPM=1 reads the exported AHB back (what SurfaceFlinger
        // gets) at named ticks as <tick>.ahb.ppm next to the snapshots.
        const char* dir = std::getenv("GE1_REPLAY_PPM_DIR");
        const char* want = std::getenv("GE1_REPLAY_AHB_PPM");
        if (!named || !pending || !dir || !want || std::strcmp(want, "1") != 0)
            return;
        ge1_gs_wait_export(pending);
        pending = 0;
        AHardwareBuffer_Desc d = {};
        AHardwareBuffer_describe(buf, &d);
        void* mem = nullptr;
        if (AHardwareBuffer_lock(buf, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr, &mem) != 0 || !mem) {
            std::cerr << "UR2: AHB lock failed tick=" << tick << '\n';
            return;
        }
        std::ofstream ppm(std::string(dir) + "/" + std::to_string(tick) + ".ahb.ppm", std::ios::binary);
        ppm << "P6\n" << w << ' ' << h << "\n255\n";
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* px = static_cast<const uint8_t*>(mem) + (uint64_t(y) * d.stride + x) * 4;
                ppm.write(reinterpret_cast<const char*>(px), 3); // R8G8B8A8: R, G, B
            }
        AHardwareBuffer_unlock(buf, nullptr);
    }
    void close()
    {
        if (pending) ge1_gs_wait_export(pending);
        for (AHardwareBuffer*& b : bufs) if (b) { ge1_gs_release_ahb(b); AHardwareBuffer_release(b); b = nullptr; }
        if (w) std::cerr << "UR1: exports=" << exports << " failures=" << failures << '\n';
    }
};
#endif

int main(int argc, char** argv)
{
    if (argc != 6) {
        std::cerr << "usage: ge1_replay CAPTURE CSV MAX_TICK BLEND SNAP_EVERY\n";
        return 2;
    }
    const uint64_t max_tick = std::stoull(argv[3]);
    const int blend = std::stoi(argv[4]);
    const uint32_t snap_every = std::stoul(argv[5]);
    std::ifstream in(argv[1], std::ios::binary);
    std::ofstream out(argv[2]);
    if (!in || !out) { std::cerr << "input/output open failed\n"; return 2; }
    char magic[8]; in.read(magic, 8);
    if (!in || std::memcmp(magic, "PS2XGSC2", 8)) { std::cerr << "not C2\n"; return 2; }
    if (!ge1_gs_open(blend)) { std::cerr << "GS open failed\n"; return 3; }
#ifdef __ANDROID__
    ExportRing ring;
    const bool exporting = ring.init();
#endif
    out << "type,tick,index,bytes,fnv64,cpu_ms,gpu_ms,width,height\n";
    std::vector<uint8_t> rec;
    std::vector<uint8_t> fifo;
    uint64_t packet_count = 0, transfer_count = 0, read_count = 0, frame_count = 0;
    double frame_gs_cpu_ms = 0.0;
    int result = 0;
    while (true) {
        uint32_t size = 0;
        in.read(reinterpret_cast<char*>(&size), 4);
        if (in.eof()) break;
        if (!in || size < 9 || size > 64u * 1024u * 1024u) { result = 4; break; }
        rec.resize(size);
        in.read(reinterpret_cast<char*>(rec.data()), size);
        if (!in) { result = 4; break; }
        const uint8_t kind = rec[0];
        const uint64_t tick = u64at(rec.data() + 1);
        if (tick > max_tick) break;
        const uint8_t* b = rec.data() + 9;
        const uint32_t n = size - 9;
        const auto start = std::chrono::steady_clock::now();
        switch (kind) {
            case 1: {
                if (n < 5 || u32at(b + 1) != n - 5 || !ge1_gs_packet(b[0], b + 5, n - 5)) result = 5;
                packet_count++;
                break;
            }
            case 2: {
                if (n != 12 || !ge1_gs_priv_write(u32at(b), u64at(b + 4))) result = 5;
                break;
            }
            case 3: {
                if (n != 28) { result = 5; break; }
                transfer_count++;
                if (u32at(b + 24) == 1) {
                    const uint64_t r = u64at(b + 16);
                    const uint32_t w = r & 0xfffu, h = (r >> 32) & 0xfffu;
                    const uint32_t bytes = 3u * w * h;
                    if (!bytes || (bytes & 15u) || bytes > 16384u) { result = 5; break; }
                    fifo.assign(bytes, 0);
                    if (!ge1_gs_read_fifo(fifo.data(), bytes / 16u)) { result = 5; break; }
                    if (listed_tick(tick)) {
                        if (const char* dir = std::getenv("GE1_REPLAY_FIFO_DIR")) {
                            const std::string path = std::string(dir) + "/fifo-" + std::to_string(tick)
                                + "-" + std::to_string(read_count + 1) + ".bin";
                            std::ofstream dump(path, std::ios::binary);
                            dump.write(reinterpret_cast<const char*>(fifo.data()), fifo.size());
                        }
                    }
                    const auto end = std::chrono::steady_clock::now();
                    const double cpu = std::chrono::duration<double, std::milli>(end - start).count();
                    frame_gs_cpu_ms += cpu;
                    out << "fifo," << tick << ',' << ++read_count << ',' << bytes << ','
                        << std::hex << hash64(fifo.data(), bytes) << std::dec << ',' << cpu << ",0,0,0\n";
                }
                break;
            }
            case 4:
                if (n != 0) result = 5;
                break;
            case 8: {
                if (n < 164 || u32at(b) != n - 164 ||
                    !ge1_gs_reset(reinterpret_cast<const uint64_t*>(b + 4), b + 164, n - 164)) result = 5;
                break;
            }
            case 10: {
                if (n != 25 || !ge1_gs_vsync(b[0], u64at(b + 1), u64at(b + 9), u64at(b + 17))) {
                    result = 5; break;
                }
                frame_count++;
#ifdef __ANDROID__
                if (exporting) ring.frame(tick, listed_tick(tick));
#endif
                const auto end = std::chrono::steady_clock::now();
                const double cpu = frame_gs_cpu_ms +
                    std::chrono::duration<double, std::milli>(end - start).count();
                frame_gs_cpu_ms = 0.0;
                const float gpu = ge1_gs_gpu_ms();
                const bool named = listed_tick(tick);
                if ((snap_every && tick % snap_every == 0) || named) {
                    uint32_t w = 0, h = 0;
                    const uint32_t* pixels = nullptr;
                    const int got = ge1_gs_snapshot(&w, &h, &pixels);
                    if (got && named) {
                        if (const char* dir = std::getenv("GE1_REPLAY_PPM_DIR")) {
                            const std::string path = std::string(dir) + "/" + std::to_string(tick) + ".ppm";
                            std::ofstream ppm(path, std::ios::binary);
                            ppm << "P6\n" << w << ' ' << h << "\n255\n";
                            for (uint64_t i = 0; i < uint64_t(w) * h; ++i) {
                                const uint32_t p = pixels[i];
                                const char rgb[3] = {char(p), char(p >> 8), char(p >> 16)};
                                ppm.write(rgb, 3);
                            }
                        }
                    }
                    out << "frame," << tick << ',' << frame_count << ','
                        << (got ? 4ull * w * h : 0ull) << ',' << std::hex
                        << (got ? hash64(reinterpret_cast<const uint8_t*>(pixels), 4ull * w * h) : 0ull)
                        << std::dec << ',' << cpu << ',' << gpu << ',' << w << ',' << h << '\n';
                } else {
                    out << "frame," << tick << ',' << frame_count << ",0,0," << cpu << ',' << gpu << ",0,0\n";
                }
                break;
            }
            default: result = 6; break;
        }
        if (result) {
            std::cerr << "unhandled/bad record kind=" << unsigned(kind) << " tick=" << tick << " status=" << result << '\n';
            break;
        }
        if (kind == 1 || kind == 2)
            frame_gs_cpu_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
    }
#ifdef __ANDROID__
    ring.close();
#endif
    ge1_gs_close();
    std::cerr << "packets=" << packet_count << " transfers=" << transfer_count
              << " fifo=" << read_count << " frames=" << frame_count << " rc=" << result << '\n';
    return result;
}
