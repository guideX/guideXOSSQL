#include "database_buffer.h"

namespace gxos {
namespace db {

BufferManager::BufferManager(DatabaseFile& file, uint32_t capacity)
    : _file(file), _capacity(capacity), _resident(0), _dirty(0), _pinned(0),
      _clock(0), _slots(nullptr) {
    if (_capacity == 0) {
        _capacity = 1;
    }
    _slots = new BufferSlot[_capacity];
}

BufferManager::~BufferManager() {
    delete[] _slots;
    _slots = nullptr;
}

BufferSlot* BufferManager::findSlot(uint64_t pageId) {
    // A slot is "occupied" when pageId != 0 (page 0 is the header page and is
    // never cached). Pinned slots still match so a page can be re-pinned.
    for (uint32_t i = 0; i < _capacity; ++i) {
        if (_slots[i].pageId == pageId) {
            return &_slots[i];
        }
    }
    return nullptr;
}

uint32_t BufferManager::findEmptySlot() const {
    for (uint32_t i = 0; i < _capacity; ++i) {
        if (_slots[i].pageId == 0) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

DbResult BufferManager::loadIntoSlot(BufferSlot& slot, uint64_t pageId) {
    DatabasePage page;
    DbResult result = _file.readPage(pageId, page);
    if (!result.isOk()) {
        return result;
    }
    slot.pageId = pageId;
    slot.page = page;
    slot.dirty = false;
    slot.pinCount = 0;
    return DbResult::ok();
}

DbResult BufferManager::evictOne() {
    uint32_t victim = 0;
    bool found = false;
    uint64_t oldest = 0;
    for (uint32_t i = 0; i < _capacity; ++i) {
        if (_slots[i].pinCount > 0) {
            continue;
        }
        if (!found || _slots[i].lastUsed < oldest) {
            found = true;
            victim = i;
            oldest = _slots[i].lastUsed;
        }
    }
    if (!found) {
        return DbResult::error(DbStatus::Internal,
                               "buffer pool exhausted: all slots pinned");
    }

    BufferSlot& slot = _slots[victim];
    if (slot.dirty) {
        DbResult result = _file.writePage(slot.page);
        if (!result.isOk()) {
            return result;
        }
        slot.dirty = false;
        --_dirty;
    }
    slot.pageId = 0;
    slot.page = DatabasePage();
    --_resident;
    return DbResult::ok();
}

DbResult BufferManager::pinPage(uint64_t pageId, BufferSlot*& outSlot) {
    outSlot = nullptr;
    if (pageId == kHeaderPageId) {
        return DbResult::error(DbStatus::InvalidArgument, "page 0 is the header page");
    }

    BufferSlot* existing = findSlot(pageId);
    if (existing != nullptr) {
        ++existing->pinCount;
        ++_pinned;
        existing->lastUsed = ++_clock;
        outSlot = existing;
        return DbResult::ok();
    }

    uint32_t index = findEmptySlot();
    if (index == 0xFFFFFFFFu) {
        DbResult result = evictOne();
        if (!result.isOk()) {
            return result;
        }
        index = findEmptySlot();
        if (index == 0xFFFFFFFFu) {
            return DbResult::error(DbStatus::Internal,
                                   "buffer pool exhausted: no slot available");
        }
    }

    DbResult result = loadIntoSlot(_slots[index], pageId);
    if (!result.isOk()) {
        return result;
    }
    _slots[index].pinCount = 1;
    _slots[index].lastUsed = ++_clock;
    ++_resident;
    ++_pinned;
    outSlot = &_slots[index];
    return DbResult::ok();
}

DbResult BufferManager::pinAllocatedPage(uint64_t pageId, BufferSlot*& outSlot) {
    outSlot = nullptr;
    if (pageId == kHeaderPageId) {
        return DbResult::error(DbStatus::InvalidArgument, "page 0 is the header page");
    }

    BufferSlot* existing = findSlot(pageId);
    if (existing != nullptr) {
        ++existing->pinCount;
        ++_pinned;
        existing->lastUsed = ++_clock;
        existing->dirty = true;
        outSlot = existing;
        return DbResult::ok();
    }

    uint32_t index = findEmptySlot();
    if (index == 0xFFFFFFFFu) {
        DbResult result = evictOne();
        if (!result.isOk()) {
            return result;
        }
        index = findEmptySlot();
        if (index == 0xFFFFFFFFu) {
            return DbResult::error(DbStatus::Internal,
                                   "buffer pool exhausted: no slot available");
        }
    }

    BufferSlot& slot = _slots[index];
    slot.pageId = pageId;
    slot.page = DatabasePage();
    slot.page.pageId = pageId;
    slot.page.type = PageType::Data;
    slot.dirty = true;
    slot.pinCount = 1;
    slot.lastUsed = ++_clock;
    ++_resident;
    ++_dirty;
    ++_pinned;
    outSlot = &slot;
    return DbResult::ok();
}

void BufferManager::markDirty(BufferSlot& slot) {
    if (!slot.dirty) {
        slot.dirty = true;
        ++_dirty;
    }
}

void BufferManager::unpin(BufferSlot& slot) {
    if (slot.pinCount > 0) {
        --slot.pinCount;
        --_pinned;
    }
}

DbResult BufferManager::flush() {
    for (uint32_t i = 0; i < _capacity; ++i) {
        if (_slots[i].dirty) {
            DbResult result = _file.writePage(_slots[i].page);
            if (!result.isOk()) {
                return result;
            }
            _slots[i].dirty = false;
            --_dirty;
        }
    }
    return _file.flush();
}

} // namespace db
} // namespace gxos
