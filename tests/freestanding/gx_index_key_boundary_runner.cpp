#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

extern "C" int gxIndexKeyBoundaryProbe(
    const uint8_t* const* leafPayloads,
    const uint64_t* leafPageIds,
    const size_t* leafLengths,
    size_t leafCount,
    uint64_t firstPageId,
    uint64_t pageCount,
    uint32_t ownerIndexId);

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
        std::cerr << "expected the R3Z production leaf-chain fixture path\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::in | std::ios::binary);
    uint64_t pageCount = 0;
    uint32_t ownerIndexId = 0;
    uint64_t firstPageId = 0;
    size_t leafCount = 0;
    if (!input || !(input >> pageCount >> ownerIndexId >> firstPageId >> leafCount) ||
        leafCount < 3 || leafCount > 1024) {
        std::cerr << "invalid R3Z fixture header\n";
        return 3;
    }

    std::vector<uint64_t> pageIds(leafCount, 0);
    std::vector<std::vector<uint8_t> > payloads(leafCount);
    for (size_t i = 0; i < leafCount; ++i) {
        std::string encoded;
        if (!(input >> pageIds[i] >> encoded) ||
            !parsePayload(encoded, payloads[i]) || payloads[i].size() != 480) {
            std::cerr << "invalid R3Z fixture leaf record " << i << "\n";
            return 4;
        }
    }
    std::string extra;
    if (input >> extra) {
        std::cerr << "unexpected trailing R3Z fixture data\n";
        return 5;
    }

    std::vector<const uint8_t*> payloadPointers(leafCount, nullptr);
    std::vector<size_t> payloadLengths(leafCount, 0);
    for (size_t i = 0; i < leafCount; ++i) {
        payloadPointers[i] = payloads[i].data();
        payloadLengths[i] = payloads[i].size();
    }
    return gxIndexKeyBoundaryProbe(
        payloadPointers.data(), pageIds.data(), payloadLengths.data(), leafCount,
        firstPageId, pageCount, ownerIndexId);
}
