#pragma once
// guideXOS SQL -- Phase SQL2
// Relational value model: the bounded SQL2 type system and the DbValue
// container used by the native table/row API.
//
// On-disk representation (little-endian, explicitly serialized):
//
//   Boolean : 1 byte, 0 = false, 1 = true
//   Int32   : 4 bytes, two's complement
//   Int64   : 8 bytes, two's complement
//   Float64 : 8 bytes, IEEE 754 binary64
//   Text    : u32 byte length followed by raw bytes (no NUL terminator,
//             no encoding conversion; the storage layer treats Text as an
//             opaque byte string)
//   Blob    : u32 byte length followed by raw bytes
//
// NULL is represented for nullable columns by the row's null bitmap; a
// DbValue with isNull() == true carries no payload.
//
// The storage layer performs no implicit coercion: a value presented for a
// column must already have exactly that column's type (or be NULL for a
// nullable column).

#include <cstdint>
#include <string>
#include <vector>

#include "database_result.h"

namespace gxos {
namespace db {

// An ephemeral, internal row locator: the physical position of a row in a heap
// chain. It is valid only for the relevant scan/database generation and is
// never a SQL-visible key. SQL6 indexes store it as their entry payload, so it
// is defined here (rather than in the heap header) to keep the relational and
// index layers free of an include cycle.
struct RowLocator {
    uint64_t pageId;
    uint32_t slot;

    RowLocator() : pageId(0), slot(0) {}
    RowLocator(uint64_t pageIdIn, uint32_t slotIn) : pageId(pageIdIn), slot(slotIn) {}
};

enum class DbType : uint16_t {
    Unknown = 0,
    Boolean = 1,
    Int32 = 2,
    Int64 = 3,
    Float64 = 4,
    Text = 5,
    Blob = 6
};

inline const char* dbTypeName(DbType type) {
    switch (type) {
    case DbType::Boolean: return "Boolean";
    case DbType::Int32: return "Int32";
    case DbType::Int64: return "Int64";
    case DbType::Float64: return "Float64";
    case DbType::Text: return "Text";
    case DbType::Blob: return "Blob";
    }
    return "Unknown";
}

// Returns true when `type` is one of the SQL2 concrete types.
inline bool isConcreteDbType(DbType type) {
    return type == DbType::Boolean || type == DbType::Int32 ||
           type == DbType::Int64 || type == DbType::Float64 ||
           type == DbType::Text || type == DbType::Blob;
}

class DbValue {
public:
    DbValue() : _type(DbType::Unknown), _null(true), _bool(false), _int32(0),
                _int64(0), _float64(0.0) {}

    static DbValue null() { return DbValue(); }

    static DbValue boolean(bool value) {
        DbValue v;
        v._type = DbType::Boolean;
        v._null = false;
        v._bool = value;
        return v;
    }

    static DbValue int32(int32_t value) {
        DbValue v;
        v._type = DbType::Int32;
        v._null = false;
        v._int32 = value;
        return v;
    }

    static DbValue int64(int64_t value) {
        DbValue v;
        v._type = DbType::Int64;
        v._null = false;
        v._int64 = value;
        return v;
    }

    static DbValue float64(double value) {
        DbValue v;
        v._type = DbType::Float64;
        v._null = false;
        v._float64 = value;
        return v;
    }

    static DbValue text(const std::string& value) {
        DbValue v;
        v._type = DbType::Text;
        v._null = false;
        v._text = value;
        return v;
    }

    static DbValue blob(const uint8_t* data, size_t length) {
        DbValue v;
        v._type = DbType::Blob;
        v._null = false;
        if (data != nullptr && length > 0) {
            v._blob.assign(data, data + length);
        }
        return v;
    }

    bool isNull() const { return _null; }
    DbType type() const { return _type; }

    bool booleanValue() const { return _bool; }
    int32_t int32Value() const { return _int32; }
    int64_t int64Value() const { return _int64; }
    double float64Value() const { return _float64; }
    const std::string& textValue() const { return _text; }
    const std::vector<uint8_t>& blobValue() const { return _blob; }

private:
    DbType _type;
    bool _null;
    bool _bool;
    int32_t _int32;
    int64_t _int64;
    double _float64;
    std::string _text;
    std::vector<uint8_t> _blob;
};

} // namespace db
} // namespace gxos
