#include "gx_index_key.h"

int r3tPortableHeaderConsumerA() {
    const uint8_t left[] = {0x00u};
    const uint8_t right[] = {0x01u};
    bool isNull = false;
    int comparison = 0;
    return gxIndexKeyIsNull(left, sizeof(left), &isNull) == GxStatus::Ok &&
                   isNull &&
                   gxCompareIndexKeys(left, sizeof(left), right, sizeof(right),
                                      &comparison) == GxStatus::Ok &&
                   comparison == -1
        ? 1 : 0;
}
