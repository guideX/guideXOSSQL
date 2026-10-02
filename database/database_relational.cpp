#include "database_relational.h"

#include <cstring>

#include "database_endian.h"
#include "database_engine.h"
#include "database_heap.h"

namespace gxos {
namespace db {

namespace {

const uint32_t kDefaultBufferCapacity = 32;

} // namespace

Database::Database()
    : _file(), _buffer(), _catalog(), _diagnostics(), _open(false), _readOnly(false),
      _catalogDirty(false) {}

Database::~Database() {
    if (_open) {
        close();
    }
}

void Database::markCatalogDirty() {
    _catalogDirty = true;
}

void Database::refreshBufferDiagnostics() {
    if (_buffer) {
        _diagnostics.bufferCapacity = _buffer->capacity();
        _diagnostics.bufferResident = _buffer->residentCount();
        _diagnostics.bufferDirty = _buffer->dirtyCount();
    } else {
        _diagnostics.bufferCapacity = 0;
        _diagnostics.bufferResident = 0;
        _diagnostics.bufferDirty = 0;
    }
}

uint32_t Database::bufferCapacity() const {
    return _buffer ? _buffer->capacity() : 0;
}

DbResult Database::openImpl(const std::string& path, const DatabaseOpenOptions& options,
                            const DatabaseCreateOptions& createOptions, bool create) {
    const uint32_t bufferCapacity =
        options.bufferCapacity != 0 ? options.bufferCapacity : kDefaultBufferCapacity;

    if (create) {
        DbResult result = DatabaseEngine::createDatabase(path, createOptions, _file);
        if (!result.isOk()) {
            return result;
        }
    } else {
        DbResult result = DatabaseEngine::openDatabase(path, options, _file);
        if (!result.isOk()) {
            return result;
        }
    }

    _buffer.reset(new BufferManager(*_file, bufferCapacity));
    _readOnly = options.readOnly;

    DbResult result = _catalog.load(*_buffer, _file->rootPageId(), _file->pageSize(),
                                    _file->pageCount());
    if (!result.isOk()) {
        _buffer.reset();
        _file->close();
        _file.reset();
        return result;
    }

    _open = true;
    _catalogDirty = false;
    refreshDiagnostics();
    return DbResult::ok();
}

DbResult Database::create(const std::string& path, const DatabaseCreateOptions& options,
                          std::unique_ptr<Database>& out) {
    if (path.empty()) {
        return DbResult::error(DbStatus::InvalidArgument, "empty database path");
    }
    if (!isSupportedPageSize(options.pageSize)) {
        return DbResult::error(DbStatus::InvalidArgument, "unsupported page size");
    }

    std::unique_ptr<Database> db(new Database());
    DatabaseCreateOptions createOptions;
    createOptions.pageSize = options.pageSize;
    createOptions.overwriteExisting = options.overwriteExisting;
    createOptions.generateDatabaseId = options.generateDatabaseId;
    std::memcpy(createOptions.databaseId, options.databaseId, 16);
    createOptions.creationTimeUnixNanos = options.creationTimeUnixNanos;
    createOptions.bufferCapacity = options.bufferCapacity;

    DatabaseOpenOptions createOpen;
    createOpen.bufferCapacity = options.bufferCapacity;
    DbResult result = db->openImpl(path, createOpen, createOptions, true);
    if (!result.isOk()) {
        return result;
    }
    out = std::move(db);
    return DbResult::ok();
}

DbResult Database::open(const std::string& path, const DatabaseOpenOptions& options,
                        std::unique_ptr<Database>& out) {
    if (path.empty()) {
        return DbResult::error(DbStatus::InvalidArgument, "empty database path");
    }

    std::unique_ptr<Database> db(new Database());
    DbResult result = db->openImpl(path, options, DatabaseCreateOptions(), false);
    if (!result.isOk()) {
        return result;
    }
    out = std::move(db);
    return DbResult::ok();
}

DbResult Database::close() {
    if (!_open) {
        return DbResult::ok();
    }
    DbResult result = DbResult::ok();
    if (!_readOnly) {
        result = flush();
    }
    _buffer.reset();
    DbResult closed = _file->close();
    _file.reset();
    _open = false;
    _catalogDirty = false;
    if (!result.isOk()) {
        return result;
    }
    return closed;
}

DbResult Database::flush() {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return DbResult::ok();
    }

    if (_catalogDirty) {
        DbResult result = _catalog.save(*_buffer, *_file, _file->rootPageId(),
                                        _file->pageSize());
        if (!result.isOk()) {
            return result;
        }
        _catalogDirty = false;
    }

    DbResult result = _buffer->flush();
    if (!result.isOk()) {
        return result;
    }
    refreshDiagnostics();
    return DbResult::ok();
}

DbResult Database::createTable(const TableDefinition& def, uint32_t& outTableId) {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    if (_readOnly) {
        return DbResult::error(DbStatus::ReadOnly, "database opened read-only");
    }

    DbResult result = _catalog.addTable(def, _file->pageSize(), outTableId);
    if (!result.isOk()) {
        return result;
    }
    _catalogDirty = true;
    refreshDiagnostics();
    return DbResult::ok();
}

DbResult Database::listTables(std::vector<TableInfo>& out) const {
    out.clear();
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    const std::vector<Catalog::TableRecord>& tables = _catalog.tables();
    out.reserve(tables.size());
    for (size_t i = 0; i < tables.size(); ++i) {
        TableInfo info;
        info.tableId = tables[i].tableId;
        info.name = tables[i].name;
        info.columnCount = static_cast<uint32_t>(tables[i].columns.size());
        info.heapPageCount = tables[i].heapPageCount;
        info.rowCount = tables[i].rowCount;
        out.push_back(info);
    }
    return DbResult::ok();
}

DbResult Database::describeTable(const std::string& name, TableDefinition& out) const {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    const Catalog::TableRecord* rec = _catalog.findTable(name);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "no such table");
    }
    out.name = rec->name;
    out.columns = rec->columns;
    return DbResult::ok();
}

DbResult Database::openTable(const std::string& name, std::unique_ptr<Table>& out) {
    if (!_open) {
        return DbResult::error(DbStatus::NotOpen, "database is not open");
    }
    const Catalog::TableRecord* rec = _catalog.findTable(name);
    if (rec == nullptr) {
        return DbResult::error(DbStatus::InvalidArgument, "no such table");
    }
    out.reset(new Table(*this, rec->tableId));
    return DbResult::ok();
}

void Database::refreshDiagnostics() {
    _diagnostics.open = _open;
    _diagnostics.readOnly = _readOnly;
    if (_file) {
        _diagnostics.formatMajor = _file->header().formatMajor;
        _diagnostics.formatMinor = _file->header().formatMinor;
        _diagnostics.pageSize = _file->pageSize();
        _diagnostics.pageCount = _file->pageCount();
        _diagnostics.rootPageId = _file->rootPageId();
        _diagnostics.databaseId = formatDatabaseId(_file->header().databaseId);
        _diagnostics.fileSizeBytes = _file->diagnostics().fileSizeBytes;
    }
    _diagnostics.tableCount = _catalog.tableCount();
    _diagnostics.catalogPageCount = _catalog.catalogPageCount();
    if (_buffer) {
        _diagnostics.bufferCapacity = _buffer->capacity();
        _diagnostics.bufferResident = _buffer->residentCount();
        _diagnostics.bufferDirty = _buffer->dirtyCount();
    } else {
        _diagnostics.bufferCapacity = 0;
        _diagnostics.bufferResident = 0;
        _diagnostics.bufferDirty = 0;
    }
    _diagnostics.tables.clear();
    const std::vector<Catalog::TableRecord>& tables = _catalog.tables();
    for (size_t i = 0; i < tables.size(); ++i) {
        TableDiagnostics td;
        td.tableId = tables[i].tableId;
        td.name = tables[i].name;
        td.columnCount = static_cast<uint32_t>(tables[i].columns.size());
        td.heapPageCount = tables[i].heapPageCount;
        td.rowCount = tables[i].rowCount;
        _diagnostics.tables.push_back(td);
    }
    if (_file) {
        _diagnostics.state = _file->diagnostics().state;
    }
}

} // namespace db
} // namespace gxos
