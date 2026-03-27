/*
 * Converts ShaderMake SPIR-V header blobs (*.spirv.h, NVSP format) to embedded Metal metallib blobs (*.metal.h).
 * Invokes spirv-cross, then xcrun metal + metallib (macOS host only).
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static constexpr char kNvsp[] = {'N', 'V', 'S', 'P'};
static constexpr size_t kNvspSize = 4;

struct ShaderBlobEntry {
    uint32_t permutationSize;
    uint32_t dataSize;
};

static bool ReadFileAll(const fs::path& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    const auto sz = f.tellg();
    f.seekg(0);
    out.resize(static_cast<size_t>(sz));
    if (sz > 0)
        f.read(out.data(), sz);
    return static_cast<bool>(f);
}

static bool ParseUint8ArrayFromHeader(const std::string& text, std::vector<uint8_t>& blob) {
    const size_t key = text.find("const uint8_t");
    if (key == std::string::npos)
        return false;
    size_t brace = text.find('{', key);
    if (brace == std::string::npos)
        return false;
    blob.clear();
    size_t i = brace + 1;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r'))
            i++;
        if (i < text.size() && text[i] == '}')
            break;
        if (i >= text.size())
            return false;
        char* end = nullptr;
        const unsigned long v = std::strtoul(text.c_str() + i, &end, 0);
        if (end == text.c_str() + i)
            return false;
        if (v > 255)
            return false;
        blob.push_back(static_cast<uint8_t>(v));
        i = static_cast<size_t>(end - text.c_str());
        while (i < text.size() && text[i] != ',' && text[i] != '}')
            i++;
        if (i < text.size() && text[i] == ',')
            i++;
    }
    return !blob.empty();
}

static std::string ExtractArraySymbol(const std::string& text) {
    const size_t key = text.find("const uint8_t");
    if (key == std::string::npos)
        return {};
    size_t lb = text.find('[', key);
    if (lb == std::string::npos)
        return {};
    size_t nameStart = key + strlen("const uint8_t");
    while (nameStart < lb && std::isspace(static_cast<unsigned char>(text[nameStart])))
        nameStart++;
    return text.substr(nameStart, lb - nameStart);
}

static int RunCommand(const std::string& cmd) {
    return std::system(cmd.c_str());
}

static bool AppendPermutationBlob(
    std::vector<uint8_t>& out,
    const std::string& permutationKey,
    const void* data,
    size_t dataSize) {
    ShaderBlobEntry e{};
    e.permutationSize = static_cast<uint32_t>(permutationKey.size());
    e.dataSize = static_cast<uint32_t>(dataSize);
    const auto* p = reinterpret_cast<const uint8_t*>(&e);
    out.insert(out.end(), p, p + sizeof(e));
    out.insert(out.end(), permutationKey.begin(), permutationKey.end());
    const auto* d = static_cast<const uint8_t*>(data);
    out.insert(out.end(), d, d + dataSize);
    return true;
}

static bool ConvertOnePermutation(
    const std::vector<uint8_t>& spirv,
    const fs::path& tmpDir,
    const std::string& spirvCrossExe,
    const std::string& metalSdkArgs,
    std::vector<uint8_t>& outMetallib) {
    const fs::path spvPath = tmpDir / "in.spv";
    const fs::path mslPath = tmpDir / "shader.metal";
    const fs::path airPath = tmpDir / "shader.air";
    const fs::path libPath = tmpDir / "shader.metallib";

    {
        std::ofstream f(spvPath, std::ios::binary);
        if (!f)
            return false;
        f.write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size()));
    }

    std::ostringstream sc;
    sc << '"' << spirvCrossExe << "\" \"" << spvPath.string() << "\" --msl --msl-version 30000 --output \"" << mslPath.string() << '"';
    if (RunCommand(sc.str()) != 0)
        return false;

    std::ostringstream mc;
    mc << "xcrun " << metalSdkArgs << " metal -fno-fast-math -std=metal3.0 -Wno-unused-const-variable -c \"" << mslPath.string() << "\" -o \"" << airPath.string() << '"';
    if (RunCommand(mc.str()) != 0)
        return false;

    std::ostringstream ml;
    ml << "xcrun " << metalSdkArgs << " metallib \"" << airPath.string() << "\" -o \"" << libPath.string() << '"';
    if (RunCommand(ml.str()) != 0)
        return false;

    std::string metallibData;
    if (!ReadFileAll(libPath, metallibData))
        return false;
    outMetallib.assign(metallibData.begin(), metallibData.end());
    return !outMetallib.empty();
}

static bool ProcessSpirvBlob(const std::vector<uint8_t>& blob, const fs::path& tmpDir, const std::string& spirvCrossExe, const std::string& metalSdkArgs, std::vector<uint8_t>& outBlob) {
    outBlob.clear();
    // Single-shader output: raw SPIR-V (no NVSP header); runtime uses the whole blob when permutation count is 0.
    if (blob.size() < kNvspSize || memcmp(blob.data(), kNvsp, kNvspSize) != 0) {
        std::vector<uint8_t> metallib;
        if (!ConvertOnePermutation(blob, tmpDir, spirvCrossExe, metalSdkArgs, metallib))
            return false;
        outBlob = std::move(metallib);
        return true;
    }

    outBlob.insert(outBlob.end(), kNvsp, kNvsp + kNvspSize);

    size_t offset = kNvspSize;
    while (offset + sizeof(ShaderBlobEntry) <= blob.size()) {
        ShaderBlobEntry header{};
        memcpy(&header, blob.data() + offset, sizeof(header));
        offset += sizeof(header);

        if (header.dataSize == 0)
            return false;
        if (offset + header.permutationSize + header.dataSize > blob.size())
            return false;

        std::string perm;
        perm.resize(header.permutationSize);
        if (header.permutationSize)
            memcpy(perm.data(), blob.data() + offset, header.permutationSize);
        offset += header.permutationSize;

        const uint8_t* spirvData = blob.data() + offset;
        offset += header.dataSize;

        std::vector<uint8_t> spirv(spirvData, spirvData + header.dataSize);

        std::vector<uint8_t> metallib;
        if (!ConvertOnePermutation(spirv, tmpDir, spirvCrossExe, metalSdkArgs, metallib))
            return false;

        if (!AppendPermutationBlob(outBlob, perm, metallib.data(), metallib.size()))
            return false;
    }
    return offset == blob.size() && !outBlob.empty();
}

static void WriteMetalHeader(const fs::path& outPath, const std::string& arrayName, const std::vector<uint8_t>& blob) {
    std::ofstream f(outPath);
    if (!f) {
        std::fprintf(stderr, "SpirvToMetallib: cannot write '%s'\n", outPath.string().c_str());
        std::exit(1);
    }
    f << "// {}\n";
    f << "const uint8_t " << arrayName << "[] = {\n    ";
    int lineLen = 0;
    for (size_t i = 0; i < blob.size(); i++) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u", blob[i]);
        f << buf << ',';
        lineLen += static_cast<int>(strlen(buf)) + 1;
        if (lineLen > 120 && i + 1 < blob.size()) {
            f << "\n    ";
            lineLen = 0;
        }
    }
    f << "\n};\n";
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "Usage: SpirvToMetallib <spirvCrossExe> <spirv.h> <out.metal.h> [xcrun-sdk-args]\n"
            "  xcrun-sdk-args: e.g. \"-sdk macosx\" (default)\n");
        return 1;
    }

    const std::string spirvCrossExe = argv[1];
    const fs::path spirvHeader = argv[2];
    const fs::path outHeader = argv[3];
    const std::string metalSdkArgs = (argc >= 5) ? argv[4] : "-sdk macosx";

    std::string text;
    if (!ReadFileAll(spirvHeader, text)) {
        std::fprintf(stderr, "SpirvToMetallib: failed to read '%s'\n", spirvHeader.string().c_str());
        return 1;
    }

    std::vector<uint8_t> spirvBlob;
    if (!ParseUint8ArrayFromHeader(text, spirvBlob)) {
        std::fprintf(stderr, "SpirvToMetallib: failed to parse uint8 array in '%s'\n", spirvHeader.string().c_str());
        return 1;
    }

    std::string sym = ExtractArraySymbol(text);
    if (sym.empty()) {
        std::fprintf(stderr, "SpirvToMetallib: could not find array symbol\n");
        return 1;
    }

    const std::string suffix = "_spirv";
    if (sym.size() > suffix.size() && sym.compare(sym.size() - suffix.size(), suffix.size(), suffix) == 0)
        sym.replace(sym.size() - suffix.size(), suffix.size(), "_metal");
    else
        sym += "_metal";

    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    fs::path tmpDir = fs::temp_directory_path() / ("nrd-metallib-" + std::to_string(dist(gen)));
    std::error_code ec;
    fs::create_directories(tmpDir, ec);
    if (ec) {
        std::fprintf(stderr, "SpirvToMetallib: temp dir failed\n");
        return 1;
    }

    std::vector<uint8_t> metalBlob;
    if (!ProcessSpirvBlob(spirvBlob, tmpDir, spirvCrossExe, metalSdkArgs, metalBlob)) {
        std::fprintf(stderr, "SpirvToMetallib: conversion failed for '%s'\n", spirvHeader.string().c_str());
        fs::remove_all(tmpDir, ec);
        return 1;
    }

    WriteMetalHeader(outHeader, sym, metalBlob);
    fs::remove_all(tmpDir, ec);
    return 0;
}
