// itch_slice: copy the first ~N bytes of an ITCH file, cut on a frame boundary.
//
//   itch_slice <in> <out> [--gb 1.5]
//
// Plain `head -c` would cut a message in half; this keeps whole frames only.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "itch/endian.hpp"
#include "itch/mapped_file.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: itch_slice <in> <out> [--gb N]\n");
        return 2;
    }
    double gb = 1.5;
    for (int i = 3; i < argc; ++i)
        if (!std::strcmp(argv[i], "--gb") && i + 1 < argc) gb = std::atof(argv[++i]);

    itch::MappedFile in(argv[1]);
    const std::size_t limit = static_cast<std::size_t>(gb * 1e9);
    std::size_t off = 0;
    std::size_t frames = 0;
    while (in.size() - off >= 2) {
        const std::size_t len = itch::load_be16(in.data() + off);
        if (off + 2 + len > in.size() || off + 2 + len > limit) break;
        off += 2 + len;
        ++frames;
    }

    std::FILE* out = std::fopen(argv[2], "wb");
    if (!out) { std::perror(argv[2]); return 1; }
    if (std::fwrite(in.data(), 1, off, out) != off) { std::perror("fwrite"); std::fclose(out); return 1; }
    std::fclose(out);
    std::printf("wrote %zu frames, %zu bytes (%.2f GB) to %s\n", frames, off, off / 1e9, argv[2]);
    return 0;
}
