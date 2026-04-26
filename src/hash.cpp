#include "hash.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace luban::hash {

namespace {

constexpr size_t kChunk = 1 << 16;  // 64 KiB，与 Python 端一致

#ifdef _WIN32

LPCWSTR bcrypt_algo_id(Algorithm a) {
    switch (a) {
        case Algorithm::Sha256: return BCRYPT_SHA256_ALGORITHM;
        case Algorithm::Sha512: return BCRYPT_SHA512_ALGORITHM;
        case Algorithm::Md5:    return BCRYPT_MD5_ALGORITHM;
        case Algorithm::Sha1:   return BCRYPT_SHA1_ALGORITHM;
    }
    return BCRYPT_SHA256_ALGORITHM;
}

// RAII 包 BCryptAlgorithmHandle / HashHandle
struct AlgorithmHandle {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~AlgorithmHandle() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};

struct HashHandle {
    BCRYPT_HASH_HANDLE h = nullptr;
    ~HashHandle() { if (h) BCryptDestroyHash(h); }
};

#endif  // _WIN32

std::string to_hex(const unsigned char* data, size_t n) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(n * 2, '\0');
    for (size_t i = 0; i < n; ++i) {
        out[2 * i]     = kHex[(data[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[data[i] & 0xF];
    }
    return out;
}

}  // namespace

std::string algo_name(Algorithm a) {
    switch (a) {
        case Algorithm::Sha256: return "sha256";
        case Algorithm::Sha512: return "sha512";
        case Algorithm::Md5:    return "md5";
        case Algorithm::Sha1:   return "sha1";
    }
    return "sha256";
}

std::optional<Algorithm> parse_algo(std::string_view name) {
    std::string s(name);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "sha256") return Algorithm::Sha256;
    if (s == "sha512") return Algorithm::Sha512;
    if (s == "md5")    return Algorithm::Md5;
    if (s == "sha1")   return Algorithm::Sha1;
    return std::nullopt;
}

std::optional<HashSpec> parse(std::string_view raw) {
    // 去首尾 ws
    size_t b = 0;
    size_t e = raw.size();
    while (b < e && std::isspace(static_cast<unsigned char>(raw[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(raw[e-1]))) --e;
    if (b == e) return std::nullopt;
    std::string trimmed(raw.substr(b, e - b));

    auto colon = trimmed.find(':');
    Algorithm algo = Algorithm::Sha256;
    std::string hex;
    if (colon != std::string::npos) {
        auto a = parse_algo(std::string_view(trimmed).substr(0, colon));
        if (!a) return std::nullopt;
        algo = *a;
        hex = trimmed.substr(colon + 1);
    } else {
        hex = trimmed;
    }
    // 验证全 hex
    if (hex.empty()) return std::nullopt;
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    std::transform(hex.begin(), hex.end(), hex.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return HashSpec{algo, hex};
}

std::string to_string(const HashSpec& spec) {
    return algo_name(spec.algo) + ":" + spec.hex;
}

std::optional<HashSpec> hash_file(const fs::path& path, Algorithm algo) {
#ifdef _WIN32
    AlgorithmHandle alg;
    if (BCryptOpenAlgorithmProvider(&alg.h, bcrypt_algo_id(algo), nullptr, 0) != 0) {
        return std::nullopt;
    }

    DWORD digest_size = 0;
    DWORD got = 0;
    if (BCryptGetProperty(alg.h, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&digest_size), sizeof(digest_size),
                          &got, 0) != 0) {
        return std::nullopt;
    }

    HashHandle hash;
    if (BCryptCreateHash(alg.h, &hash.h, nullptr, 0, nullptr, 0, 0) != 0) {
        return std::nullopt;
    }

    std::FILE* fp = nullptr;
#ifdef _MSC_VER
    if (_wfopen_s(&fp, path.wstring().c_str(), L"rb") != 0) fp = nullptr;
#else
    fp = _wfopen(path.wstring().c_str(), L"rb");
#endif
    if (!fp) return std::nullopt;

    std::vector<unsigned char> buf(kChunk);
    while (true) {
        size_t n = std::fread(buf.data(), 1, buf.size(), fp);
        if (n == 0) break;
        if (BCryptHashData(hash.h, buf.data(), static_cast<ULONG>(n), 0) != 0) {
            std::fclose(fp);
            return std::nullopt;
        }
        if (n < buf.size()) break;
    }
    std::fclose(fp);

    std::vector<unsigned char> digest(digest_size);
    if (BCryptFinishHash(hash.h, digest.data(), digest_size, 0) != 0) {
        return std::nullopt;
    }
    return HashSpec{algo, to_hex(digest.data(), digest.size())};
#else
    (void)path;
    (void)algo;
    return std::nullopt;  // POSIX 在 M2 跨平台时再加（OpenSSL / mbedTLS）
#endif
}

bool verify_file(const fs::path& path, const HashSpec& expected) {
    auto actual = hash_file(path, expected.algo);
    if (!actual) return false;
    // hex 已是小写
    return actual->hex == expected.hex;
}

}  // namespace luban::hash
