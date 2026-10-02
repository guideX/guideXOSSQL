#include "database_checksum.h"

namespace gxos {
namespace db {

namespace {

struct Crc32Table {
    uint32_t values[256];
    Crc32Table() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            values[i] = c;
        }
    }
};

const Crc32Table& table() {
    static const Crc32Table instance;
    return instance;
}

} // namespace

uint32_t crc32Init() {
    return 0xFFFFFFFFu;
}

uint32_t crc32Update(uint32_t state, const uint8_t* data, size_t length) {
    if (data == nullptr || length == 0) {
        return state;
    }
    const Crc32Table& t = table();
    uint32_t crc = state;
    for (size_t i = 0; i < length; ++i) {
        crc = t.values[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

uint32_t crc32Final(uint32_t state) {
    return state ^ 0xFFFFFFFFu;
}

uint32_t crc32(const uint8_t* data, size_t length) {
    return crc32Final(crc32Update(crc32Init(), data, length));
}

} // namespace db
} // namespace gxos
