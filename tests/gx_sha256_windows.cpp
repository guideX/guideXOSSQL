#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <vector>

extern "C" int gxR3agSha256File(const char* path, char* outHex65) {
    if (!path || !outHex65) return 0;
    outHex65[0] = '\0';

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD resultLength = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) < 0) {
        return 0;
    }
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &resultLength, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashLength),
                          sizeof(hashLength), &resultLength, 0) < 0 ||
        objectLength == 0 || hashLength != 32) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return 0;
    }

    std::vector<uint8_t> hashObject(objectLength);
    std::vector<uint8_t> digest(hashLength);
    if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength,
                         nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return 0;
    }

    std::ifstream input(path, std::ios::binary);
    char chunk[8192];
    bool ok = input.good();
    while (ok && input) {
        input.read(chunk, sizeof(chunk));
        const std::streamsize count = input.gcount();
        if (count > 0 && BCryptHashData(hash,
                reinterpret_cast<PUCHAR>(chunk),
                static_cast<ULONG>(count), 0) < 0) {
            ok = false;
        }
    }
    if (!input.eof()) ok = false;
    if (ok && BCryptFinishHash(hash, digest.data(), hashLength, 0) >= 0) {
        static const char kHex[] = "0123456789abcdef";
        for (size_t i = 0; i < digest.size(); ++i) {
            outHex65[i * 2] = kHex[digest[i] >> 4];
            outHex65[i * 2 + 1] = kHex[digest[i] & 0x0f];
        }
        outHex65[64] = '\0';
    } else {
        ok = false;
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok ? 1 : 0;
}
