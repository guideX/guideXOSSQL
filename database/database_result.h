#pragma once
// guideXOS SQL -- Phase SQL1
// Bounded, inspectable result/error type for the database storage engine.
//
// The engine deliberately avoids exceptions. Every fallible operation returns
// a DbResult carrying a small status enum plus a human readable, bounded
// message. This keeps failure classes easy to branch on without growing a
// large exception hierarchy.

#include <string>
#include <utility>

namespace gxos {
namespace db {

enum class DbStatus : int {
    Ok = 0,
    InvalidArgument,
    NotDatabase,
    UnsupportedVersion,
    Truncated,
    CorruptHeader,
    CorruptPage,
    IoError,
    OutOfBounds,
    NoSpace,
    NotOpen,
    AlreadyExists,
    ReadOnly,
    Internal,
    // ---- Phase SQL3: transactions and write-ahead logging ----------------
    TransactionAlreadyActive,
    NoActiveTransaction,
    TransactionTooLarge,
    WalCorrupt,
    WalUnsupportedVersion,
    WalDatabaseMismatch,
    RecoveryRequired,
    RecoveryFailed,
    CommitFailed,
    RollbackFailed,
    // ---- Phase SQL8: foreign keys and schema lifecycle -------------------
    ForeignKeyViolation,
    DependencyExists
};

inline const char* dbStatusName(DbStatus status) {
    switch (status) {
    case DbStatus::Ok: return "Ok";
    case DbStatus::InvalidArgument: return "InvalidArgument";
    case DbStatus::NotDatabase: return "NotDatabase";
    case DbStatus::UnsupportedVersion: return "UnsupportedVersion";
    case DbStatus::Truncated: return "Truncated";
    case DbStatus::CorruptHeader: return "CorruptHeader";
    case DbStatus::CorruptPage: return "CorruptPage";
    case DbStatus::IoError: return "IoError";
    case DbStatus::OutOfBounds: return "OutOfBounds";
    case DbStatus::NoSpace: return "NoSpace";
    case DbStatus::NotOpen: return "NotOpen";
    case DbStatus::AlreadyExists: return "AlreadyExists";
    case DbStatus::ReadOnly: return "ReadOnly";
    case DbStatus::Internal: return "Internal";
    case DbStatus::TransactionAlreadyActive: return "TransactionAlreadyActive";
    case DbStatus::NoActiveTransaction: return "NoActiveTransaction";
    case DbStatus::TransactionTooLarge: return "TransactionTooLarge";
    case DbStatus::WalCorrupt: return "WalCorrupt";
    case DbStatus::WalUnsupportedVersion: return "WalUnsupportedVersion";
    case DbStatus::WalDatabaseMismatch: return "WalDatabaseMismatch";
    case DbStatus::RecoveryRequired: return "RecoveryRequired";
    case DbStatus::RecoveryFailed: return "RecoveryFailed";
    case DbStatus::CommitFailed: return "CommitFailed";
    case DbStatus::RollbackFailed: return "RollbackFailed";
    case DbStatus::ForeignKeyViolation: return "ForeignKeyViolation";
    case DbStatus::DependencyExists: return "DependencyExists";
    }
    return "Unknown";
}

// A bounded result value. `message` is intended for diagnostics/logging, never
// for control flow; branch on `status`.
class DbResult {
public:
    DbResult() : _status(DbStatus::Ok) {}
    DbResult(DbStatus status, std::string message)
        : _status(status), _message(std::move(message)) {}

    static DbResult ok() { return DbResult(); }

    static DbResult error(DbStatus status, const std::string& message) {
        return DbResult(status, message);
    }

    bool isOk() const { return _status == DbStatus::Ok; }
    explicit operator bool() const { return isOk(); }

    DbStatus status() const { return _status; }
    const std::string& message() const { return _message; }

    // Renders "Status: message" for diagnostics.
    std::string describe() const {
        std::string out = dbStatusName(_status);
        if (!_message.empty()) {
            out += ": ";
            out += _message;
        }
        return out;
    }

private:
    DbStatus _status;
    std::string _message;
};

} // namespace db
} // namespace gxos
