#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

extern "C" int gxIndexInternalDescentProbe(
    const uint8_t* const* payloads, const uint64_t* pageIds,
    const uint16_t* pageTypes, const size_t* lengths, size_t pageTotal,
    uint64_t pageCount, uint32_t ownerIndexId, uint64_t rootPageId,
    uint32_t expectedDuplicateCount);

extern "C" int gxIndexCompleteLookupProbe(
    const uint8_t* const* payloads, const uint64_t* pageIds,
    const uint16_t* pageTypes, const size_t* lengths, size_t pageTotal,
    uint64_t pageCount, uint32_t ownerIndexId, uint64_t rootPageId,
    uint32_t expectedDuplicateCount);

namespace {

int hexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parsePayload(const std::string& encoded, std::vector<uint8_t>& output) {
    if ((encoded.size() & 1u) != 0) return false;
    output.clear();
    output.reserve(encoded.size() / 2);
    for (size_t i = 0; i < encoded.size(); i += 2) {
        const int high = hexDigit(encoded[i]);
        const int low = hexDigit(encoded[i + 1]);
        if (high < 0 || low < 0) return false;
        output.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "expected the R3AA production internal-tree fixture path\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::in | std::ios::binary);
    uint32_t pageSize = 0;
    uint64_t pageCount = 0;
    uint32_t ownerIndexId = 0;
    uint64_t rootPageId = 0;
    uint32_t expectedDuplicateCount = 0;
    size_t pageTotal = 0;
    if (!input || !(input >> pageSize >> pageCount >> ownerIndexId >>
                    rootPageId >> expectedDuplicateCount >> pageTotal) ||
        pageSize != 512 || pageCount <= pageTotal || pageTotal < 3 ||
        pageTotal > 4096 || rootPageId == 0 || expectedDuplicateCount < 2) {
        std::cerr << "invalid R3AA fixture header\n";
        return 3;
    }

    std::vector<uint64_t> pageIds(pageTotal, 0);
    std::vector<uint16_t> pageTypes(pageTotal, 0);
    std::vector<std::vector<uint8_t> > payloads(pageTotal);
    for (size_t i = 0; i < pageTotal; ++i) {
        std::string encoded;
        if (!(input >> pageIds[i] >> pageTypes[i] >> encoded) ||
            pageIds[i] == 0 || pageIds[i] >= pageCount ||
            !parsePayload(encoded, payloads[i]) ||
            payloads[i].size() != pageSize - 32) {
            std::cerr << "invalid R3AA fixture page record " << i << "\n";
            return 4;
        }
    }
    std::string extra;
    if (input >> extra) {
        std::cerr << "unexpected trailing R3AA fixture data\n";
        return 5;
    }

    std::vector<const uint8_t*> payloadPointers(pageTotal, nullptr);
    std::vector<size_t> lengths(pageTotal, 0);
    for (size_t i = 0; i < pageTotal; ++i) {
        payloadPointers[i] = payloads[i].data();
        lengths[i] = payloads[i].size();
    }
    const int result = gxIndexInternalDescentProbe(
        payloadPointers.data(), pageIds.data(), pageTypes.data(), lengths.data(),
        pageTotal, pageCount, ownerIndexId, rootPageId,
        expectedDuplicateCount);
    if (result != 0) std::cerr << "R3AA freestanding probe step " << result << " failed\n";
    if (result != 0) return result;
    const int lookupResult = gxIndexCompleteLookupProbe(
        payloadPointers.data(), pageIds.data(), pageTypes.data(), lengths.data(),
        pageTotal, pageCount, ownerIndexId, rootPageId,
        expectedDuplicateCount);
    if (lookupResult != 0) {
        std::cerr << "R3AB freestanding complete lookup step "
                  << lookupResult << " failed\n";
    }
    return lookupResult;
}
