#pragma once
// guideXOS SQL -- Phase SQL6
// Persistent single-column B+ tree indexes.
//
// This module is a relational/storage-layer facility: it knows nothing about
// SQL syntax. It maps a canonical, order-preserving encoding of one column's
// value to a physical RowLocator, and stores those (key, locator) pairs in a
// real multi-level B+ tree whose pages live in the ordinary PageAccess
// (transaction-private) address space.
//
// Design summary (see docs/SQL6_BTREE_INDEXES_CONSTRAINTS.md):
//
//   * A physical key is `logicalKeyBytes || locatorBytes` where the locator is
//     (u64 pageId, u32 slot) big-endian. All entries with the same logical key
//     therefore form a contiguous range, ordered by locator. This is the
//     physical tie-breaker required for duplicate logical keys.
//   * Encoded logical keys compare byte-wise (unsigned, lexicographic) in
//     exactly the SQL5 logical order: NULL < FALSE < TRUE, numeric order for
//     Int32/Int64/Float64, and unsigned UTF-8 byte order for Text.
//   * Internal nodes store, for every child, the child's lower-bound key (the
//     smallest physical key present when the child was created or last split).
//     Deletion never merges nodes, so a separator may become a conservative
//     lower bound; search remains correct because lower bounds never decrease.
//   * The root page id is owned by the durable catalog index record.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "database_format.h"
#include "database_heap.h"
#include "database_pagestore.h"
#include "database_result.h"
#include "database_schema.h"
#include "database_types.h"

namespace gxos {
namespace db {

// ---- Canonical key encoding ------------------------------------------------

// Encodes one logical value into an order-preserving byte string.
// NULL encodes to a single 0x00 byte; every non-NULL value encodes to a 0x01
// tag followed by a canonical big-endian payload. Returns false for types that
// are not indexable (Blob) or for non-indexable values (non-finite Float64).
bool encodeIndexKey(DbType type, const DbValue& value, std::vector<uint8_t>& out);

// True when the encoded key represents NULL.
bool encodedKeyIsNull(const uint8_t* key, size_t length);

// Unsigned lexicographic comparison of two encoded keys.
int compareEncodedKeys(const uint8_t* a, size_t aLength, const uint8_t* b,
                       size_t bLength);

// Decodes an encoded key back into a DbValue for diagnostics/validation.
// Returns false when the bytes are not a valid encoding for `type`.
bool decodeIndexKey(DbType type, const uint8_t* key, size_t length, DbValue& out);

// ---- Entries and results ---------------------------------------------------

struct IndexEntry {
    std::vector<uint8_t> key; // encoded logical key
    RowLocator locator;

    IndexEntry() : key(), locator() {}
};

struct IndexRangeBound {
    bool hasLower;
    bool lowerInclusive;
    DbValue lower;
    bool hasUpper;
    bool upperInclusive;
    DbValue upper;

    IndexRangeBound()
        : hasLower(false), lowerInclusive(true), lower(),
          hasUpper(false), upperInclusive(true), upper() {}
};

// Result of a structural validation pass. `ok` is false on any structural
// fault; `message` is a bounded human-readable reason.
struct IndexValidation {
    bool ok;
    std::string message;
    uint64_t entryCount;
    uint32_t height;
    uint32_t pagesVisited;

    IndexValidation() : ok(false), entryCount(0), height(0), pagesVisited(0) {}
};

// ---- B+ tree ---------------------------------------------------------------

class IndexTree {
public:
    // `pages` is the transaction-private page view; `keyType` is the indexed
    // column type; `indexId` is the durable index id recorded in every page so
    // traversal can reject pages that belong to another index.
    IndexTree(PageAccess& pages, DbType keyType, uint32_t indexId);

    // Allocates a fresh empty leaf page and returns its id. Used to seed a new
    // index before its catalog record is persisted.
    DbResult createEmpty(uint64_t& outRootPageId);

    // Returns every entry whose logical key equals `logicalKey`, in locator
    // order. `out` is cleared first.
    DbResult lookupAll(const std::vector<uint8_t>& logicalKey, uint64_t rootPageId,
                       std::vector<IndexEntry>& out) const;

    // Returns the first entry (if any) whose logical key equals `logicalKey`.
    DbResult lookupFirst(const std::vector<uint8_t>& logicalKey, uint64_t rootPageId,
                         bool& found, IndexEntry& out) const;

    // Inserts one entry. `unique` requests a uniqueness check against other
    // rows (NULL keys never conflict). `outRootPageId` receives the possibly
    // changed root page id; `outInserted` is false when the exact (key, locator)
    // entry already existed. Returns AlreadyExists on a unique violation.
    DbResult insert(const std::vector<uint8_t>& logicalKey, const RowLocator& locator,
                    uint64_t rootPageId, bool unique, uint64_t& outRootPageId,
                    bool& outInserted);

    // Removes the entry exactly matching (logicalKey, locator). `outRemoved`
    // reports whether an entry was found. The root never shrinks in SQL6.
    DbResult remove(const std::vector<uint8_t>& logicalKey, const RowLocator& locator,
                    uint64_t rootPageId, uint64_t& outRootPageId, bool& outRemoved);

    // Collects every entry satisfying `bound`, in physical order. `out` is
    // cleared first. Bounded by the number of entries in the tree.
    DbResult rangeScan(const IndexRangeBound& bound, uint64_t rootPageId,
                       std::vector<IndexEntry>& out) const;

    // Full structural validation of the tree rooted at `rootPageId`.
    DbResult validate(uint64_t rootPageId, IndexValidation& out) const;

    DbType keyType() const { return _keyType; }
    uint32_t indexId() const { return _indexId; }

    // Physical key helpers (exposed for tests and diagnostics).
    static std::vector<uint8_t> makePhysicalKey(const std::vector<uint8_t>& logicalKey,
                                                const RowLocator& locator);
    static void appendLocator(std::vector<uint8_t>& out, const RowLocator& locator);

private:
    struct Child {
        uint64_t pageId;
        std::vector<uint8_t> lowerBound; // physical key; empty means -infinity
        Child() : pageId(0), lowerBound() {}
    };

    struct LeafInfo {
        uint64_t nextLeaf;
        uint32_t entryCount;
        uint32_t ownerIndexId;
        LeafInfo() : nextLeaf(0), entryCount(0), ownerIndexId(0) {}
    };

    struct InternalInfo {
        uint32_t childCount;
        uint32_t ownerIndexId;
        InternalInfo() : childCount(0), ownerIndexId(0) {}
    };

    uint32_t capacity() const;
    static uint64_t leafUsed(const std::vector<IndexEntry>& entries);
    uint64_t internalUsed(const std::vector<Child>& children) const;
    DbResult readLeaf(uint64_t pageId, DatabasePage& page, LeafInfo& info) const;
    DbResult readLeafEntries(const DatabasePage& page, const LeafInfo& info,
                             std::vector<IndexEntry>& out) const;
    DbResult writeLeaf(uint64_t pageId, uint64_t nextLeaf,
                       const std::vector<IndexEntry>& entries);
    DbResult allocateLeaf(uint64_t nextLeaf, uint64_t& outPageId);
    DbResult readInternal(uint64_t pageId, DatabasePage& page, InternalInfo& info) const;
    DbResult readInternalChildren(const DatabasePage& page, const InternalInfo& info,
                                  std::vector<Child>& out) const;
    DbResult writeInternal(uint64_t pageId, const std::vector<Child>& children);
    DbResult allocateInternal(uint64_t& outPageId);

    // Descends from `rootPageId` to the leaf that would contain `physicalKey`,
    // recording the internal pages visited in `path`.
    DbResult findLeaf(uint64_t rootPageId, const std::vector<uint8_t>& physicalKey,
                      uint64_t& outLeafPageId, std::vector<uint64_t>& outPath) const;

    // Returns the index of the child to follow for `physicalKey`.
    static size_t chooseChild(const std::vector<Child>& children,
                              const std::vector<uint8_t>& physicalKey);

    DbResult splitRootLeaf(uint64_t oldLeaf, uint64_t newLeaf,
                           const std::vector<uint8_t>& separator,
                           uint64_t& outRootPageId);

    // Recursive structural validator used by validate().
    DbResult validateNode(uint64_t pageId, uint32_t depth, std::set<uint64_t>& visited,
                          uint64_t& entryCount, uint32_t& outHeight,
                          uint32_t& pagesVisited, std::string& error) const;

    PageAccess& _pages;
    DbType _keyType;
    uint32_t _indexId;
};

} // namespace db
} // namespace gxos
