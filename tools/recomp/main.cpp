#include <toml++/toml.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "analysis.h"
#include "codegen.h"

namespace fs = std::filesystem;

static uint32_t ParseAddr(std::string_view s) { return uint32_t(std::stoul(std::string(s), nullptr, 0)); }

static bool LoadConfig(const fs::path& path, Config& cfg) {
    toml::table t;
    try {
        t = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "%s: %s\n", path.string().c_str(), std::string(e.description()).c_str());
        return false;
    }
    fs::path dir = path.parent_path();
    auto includes = t["includes"].as_array();
    if (includes)
        for (auto& i : *includes)
            if (auto s = i.value<std::string>())
                if (!LoadConfig(dir / *s, cfg)) return false;
    if (auto v = t["xbe"]["path"].value<std::string>()) cfg.xbe_path = *v;
    if (auto v = t["xbe"]["out"].value<std::string>()) cfg.out_dir = *v;
    if (auto v = t["xbe"]["partitions"].value<int64_t>()) cfg.partitions = uint32_t(*v);
    if (auto v = t["xbe"]["gap_fill"].value<bool>()) cfg.gap_fill = *v;
    if (auto v = t["xbe"]["pointer_seeds"].value<bool>()) cfg.pointer_seeds = *v;
    if (auto a = t["xbe"]["code_sections"].as_array())
        for (auto& i : *a)
            if (auto s = i.value<std::string>()) cfg.code_sections.push_back(*s);
    if (auto a = t["xbe"]["code_prefixes"].as_array())
        for (auto& i : *a)
            if (auto s = i.value<std::string>()) cfg.code_prefixes.push_back(*s);
    if (auto a = t["xbe"]["data_ranges"].as_array())
        for (auto& i : *a)
            if (auto r = i.as_array(); r && r->size() == 2)
                cfg.data_ranges.push_back({uint32_t(r->get(0)->value<int64_t>().value_or(0)),
                                           uint32_t(r->get(1)->value<int64_t>().value_or(0))});
    if (auto f = t["functions"].as_table())
        for (auto& [k, v] : *f) cfg.functions[ParseAddr(k.str())] = v.value<std::string>().value_or("");
    for (auto [key, out] : {std::pair{"symbols", &cfg.symbols}, std::pair{"data", &cfg.data}})
        if (auto f = t[key].as_table())
            for (auto& [k, v] : *f) (*out)[std::string(k.str())] = uint32_t(v.value<int64_t>().value_or(0));
    for (auto& [name, va] : cfg.symbols)
        if (va) cfg.functions.emplace(va, name);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: xbe_recomp <config.toml> [xbe override] [out override]\n");
        return 1;
    }
    Config cfg;
    fs::path cfg_path = argv[1];
    if (!LoadConfig(cfg_path, cfg)) return 1;
    fs::path root = cfg_path.parent_path().parent_path();
    fs::path xbe_path = argc > 2 ? fs::path(argv[2]) : root / cfg.xbe_path;
    fs::path out_dir = argc > 3 ? fs::path(argv[3]) : root / cfg.out_dir;

    std::ifstream f(xbe_path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", xbe_path.string().c_str());
        return 1;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    xbe::Image img;
    std::string err;
    if (!img.Parse(std::move(data), err)) {
        std::fprintf(stderr, "%s: %s\n", xbe_path.string().c_str(), err.c_str());
        return 1;
    }
    std::fprintf(stderr, "%s: title %08X entry %08X kernel thunk %08X\n", xbe_path.filename().string().c_str(),
                 img.title_id, img.entry, img.kernel_thunk);

    Analysis an(img, cfg);
    an.Run();
    fs::create_directories(out_dir);
    CodeGen gen(an, out_dir);
    return gen.Run() ? 0 : 1;
}
