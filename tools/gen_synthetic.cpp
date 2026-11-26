// gen_synthetic: write a deterministic synthetic ITCH 5.0 file.
//
//   gen_synthetic <out> [--messages N] [--stocks K] [--seed S]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "itch/synthetic.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gen_synthetic <out> [--messages N] [--stocks K] [--seed S]\n");
        return 2;
    }
    itch::SyntheticConfig cfg;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--messages") && i + 1 < argc) cfg.messages = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--stocks") && i + 1 < argc) cfg.stocks = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) cfg.seed = std::strtoull(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (cfg.stocks == 0 || cfg.stocks > 65535) { std::fprintf(stderr, "--stocks must be 1..65535\n"); return 2; }

    std::FILE* out = std::fopen(argv[1], "wb");
    if (!out) { std::perror(argv[1]); return 1; }

    itch::SyntheticFeed feed(cfg);
    std::vector<std::uint8_t> buf;
    buf.reserve(64 << 20);
    std::size_t total = 0;
    auto flush = [&] {
        if (std::fwrite(buf.data(), 1, buf.size(), out) != buf.size()) { std::perror("fwrite"); std::exit(1); }
        total += buf.size();
        buf.clear();
    };
    feed.write_preamble(buf);
    while (!feed.done()) {
        feed.write_messages(buf, 1'000'000);
        flush();
    }
    feed.write_epilogue(buf);
    flush();
    std::fclose(out);
    std::printf("wrote %llu order messages for %u stocks (%.2f MB) to %s\n",
                static_cast<unsigned long long>(cfg.messages), cfg.stocks, total / 1e6, argv[1]);
    return 0;
}
