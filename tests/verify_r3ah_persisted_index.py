#!/usr/bin/env python3
"""Independently inspect the persisted R3AH multilevel image and index trees.

This utility intentionally reads raw fixture bytes with Python's struct and
zlib modules. It does not call the production SQL service, portable codecs, or
the query implementation. It follows the v3 catalog continuation chain,
resolves index types through persisted table columns, validates page CRCs,
walks each internal-node tree, and emits ordered key/locator groups.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path


TYPE_NAMES = {1: "Boolean", 2: "Int32", 3: "Int64", 4: "Float64", 5: "Text", 6: "Blob"}
PAGE_SIZE = 512
PAGE_HEADER_SIZE = 32
INDEX_LEAF = 3
INDEX_INTERNAL = 4
CATALOG = 1


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def checked_page(image: bytes, page_id: int, page_size: int, expected_type: int | None = None) -> tuple[bytes, int, int]:
    start = page_id * page_size
    page = image[start : start + page_size]
    if len(page) != page_size or u64(page, 0) != page_id:
        raise ValueError(f"page {page_id}: invalid extent or page identity")
    stored_crc = u32(page, 24)
    crc_page = bytearray(page)
    crc_page[24:28] = b"\0\0\0\0"
    if zlib.crc32(crc_page) & 0xFFFFFFFF != stored_crc:
        raise ValueError(f"page {page_id}: checksum mismatch")
    page_type = u16(page, 8)
    payload_size = u32(page, 12)
    if payload_size > page_size - PAGE_HEADER_SIZE:
        raise ValueError(f"page {page_id}: payload exceeds page")
    if expected_type is not None and page_type != expected_type:
        raise ValueError(f"page {page_id}: expected type {expected_type}, got {page_type}")
    return page[PAGE_HEADER_SIZE:], page_type, payload_size


def parse_table(payload: bytes, offset: int) -> tuple[int, dict]:
    table_id = u32(payload, offset)
    schema_version = u32(payload, offset + 4)
    column_count = u32(payload, offset + 8)
    first_heap = u64(payload, offset + 12)
    heap_pages = u32(payload, offset + 20)
    row_count = u64(payload, offset + 24)
    name_length = u32(payload, offset + 36)
    offset += 40
    name = payload[offset : offset + name_length].decode("utf-8")
    offset += name_length
    columns = []
    for _ in range(column_count):
        ordinal = u32(payload, offset)
        type_id = u16(payload, offset + 4)
        nullable = u16(payload, offset + 6)
        column_name_length = u32(payload, offset + 12)
        offset += 16
        column_name = payload[offset : offset + column_name_length].decode("utf-8")
        offset += column_name_length
        columns.append({"ordinal": ordinal, "name": column_name, "type": type_id, "nullable": bool(nullable)})
    if not table_id or not schema_version or len(columns) != column_count:
        raise ValueError("invalid persisted table record")
    if heap_pages == 0 and first_heap != 0:
        raise ValueError("invalid persisted table heap pointer")
    return offset, {"id": table_id, "name": name, "rows": row_count, "columns": columns}


def parse_index(payload: bytes, offset: int) -> tuple[int, dict]:
    index_id = u32(payload, offset)
    table_id = u32(payload, offset + 4)
    column_ordinal = u32(payload, offset + 8)
    root_page_id = u64(payload, offset + 12)
    flags = u16(payload, offset + 20)
    format_version = u16(payload, offset + 22)
    entry_count = u64(payload, offset + 24)
    name_length = u32(payload, offset + 32)
    offset += 36
    name = payload[offset : offset + name_length].decode("utf-8")
    offset += name_length
    if not index_id or not table_id or not format_version or not name:
        raise ValueError("invalid persisted index record")
    return offset, {
        "id": index_id,
        "name": name,
        "table_id": table_id,
        "column_ordinal": column_ordinal,
        "root": root_page_id,
        "flags": flags,
        "format_version": format_version,
        "entry_count": entry_count,
    }


def parse_catalog(image: bytes, page_size: int, page_count: int, root_page: int) -> tuple[list[dict], list[dict], list[int]]:
    payload, page_type, _ = checked_page(image, root_page, page_size, CATALOG)
    if u32(payload, 0) != 0x54435847 or u16(payload, 4) != 3:
        raise ValueError("catalog is not the expected persisted v3 format")
    table_count = u32(payload, 8)
    continuation_count = u32(payload, 16)
    continuation = u32(payload, 20)
    root_record_count = u32(payload, 24)
    index_count = u32(payload, 28)
    tables: list[dict] = []
    indexes: list[dict] = []
    continuation_ids: list[int] = []

    def records(record_payload: bytes, offset: int, count: int) -> None:
        for _ in range(count):
            kind = record_payload[offset]
            offset += 1
            if kind == 1:
                offset, table = parse_table(record_payload, offset)
                tables.append(table)
            elif kind == 2:
                offset, index = parse_index(record_payload, offset)
                indexes.append(index)
            else:
                raise ValueError(f"unsupported catalog record type {kind}")

    records(payload, 40, root_record_count)
    for _ in range(continuation_count):
        if not continuation or continuation >= page_count or continuation in continuation_ids:
            raise ValueError("invalid or cyclic catalog continuation")
        continuation_ids.append(continuation)
        continuation_payload, _, _ = checked_page(image, continuation, page_size, CATALOG)
        if u32(continuation_payload, 0) != 0x54435847 or u16(continuation_payload, 4) != 3:
            raise ValueError(f"catalog continuation {continuation}: version/signature mismatch")
        next_page = u64(continuation_payload, 8)
        records(continuation_payload, 24, u32(continuation_payload, 16))
        continuation = next_page
    if continuation or len(tables) != table_count or len(indexes) != index_count:
        raise ValueError("catalog record or continuation counts do not match")
    return tables, indexes, continuation_ids


def internal_children(payload: bytes, owner: int, page_count: int) -> list[int]:
    if u32(payload, 0) != 0x4E495847 or u16(payload, 4) != 1 or u32(payload, 20) != owner:
        raise ValueError(f"index {owner}: malformed or foreign internal node")
    count = u32(payload, 16)
    if count < 2 or count > (PAGE_SIZE - PAGE_HEADER_SIZE - 24) // 20:
        raise ValueError(f"index {owner}: impossible internal child count")
    children = []
    previous_separator = None
    for position in range(count):
        slot = PAGE_SIZE - PAGE_HEADER_SIZE - (position + 1) * 8
        offset, length = u32(payload, slot), u32(payload, slot + 4)
        if offset < 24 or length < 12 or offset + length > PAGE_SIZE - PAGE_HEADER_SIZE:
            raise ValueError(f"index {owner}: internal entry outside page")
        record = payload[offset : offset + length]
        child = u64(record, 0)
        separator_length = u32(record, 8)
        separator = record[12 : 12 + separator_length]
        if child == 0 or child >= page_count or len(separator) != separator_length or length != 12 + separator_length:
            raise ValueError(f"index {owner}: malformed child record")
        if position and (separator_length < 12 or (previous_separator is not None and separator <= previous_separator)):
            raise ValueError(f"index {owner}: invalid separator ordering")
        previous_separator = separator
        children.append(child)
    return children


def leaf_entries(payload: bytes, owner: int) -> tuple[int, list[tuple[bytes, tuple[int, int]]]]:
    if u32(payload, 0) != 0x4C495847 or u16(payload, 4) != 1 or u32(payload, 20) != owner:
        raise ValueError(f"index {owner}: malformed or foreign leaf")
    next_leaf = u64(payload, 8)
    count = u32(payload, 16)
    rows = []
    for position in range(count):
        slot = PAGE_SIZE - PAGE_HEADER_SIZE - (position + 1) * 8
        offset, length = u32(payload, slot), u32(payload, slot + 4)
        if offset < 24 or length < 16 or offset + length > PAGE_SIZE - PAGE_HEADER_SIZE:
            raise ValueError(f"index {owner}: leaf entry outside page")
        record = payload[offset : offset + length]
        key_length = u32(record, 0)
        key = record[4 : 4 + key_length]
        if len(key) != key_length or length != 16 + key_length:
            raise ValueError(f"index {owner}: malformed leaf key length")
        locator = (u64(record, 4 + key_length), u32(record, 12 + key_length))
        rows.append((key, locator))
    return next_leaf, rows


def decode_key(encoded: bytes, type_id: int) -> str:
    if encoded == b"\0":
        return "NULL"
    if not encoded or encoded[0] != 1:
        raise ValueError(f"malformed persisted logical key: {encoded.hex()}")
    body = encoded[1:]
    if type_id == 1:
        if len(body) != 1 or body[0] not in (0, 1):
            raise ValueError(f"malformed Boolean key: {encoded.hex()}")
        return "false" if body[0] == 0 else "true"
    if type_id == 2:
        if len(body) != 4:
            raise ValueError(f"malformed Int32 key: {encoded.hex()}")
        return str(struct.unpack(">i", bytes([body[0] ^ 0x80]) + body[1:])[0])
    if type_id == 3:
        if len(body) != 8:
            raise ValueError(f"malformed Int64 key: {encoded.hex()}")
        return str(struct.unpack(">q", bytes([body[0] ^ 0x80]) + body[1:])[0])
    if type_id == 5:
        return body.hex()
    return encoded.hex()


def index_tree(image: bytes, page_size: int, page_count: int, index: dict) -> dict:
    owner = index["id"]
    seen: set[int] = set()
    leaves: list[tuple[int, int, list[tuple[bytes, tuple[int, int]]]]] = []
    internal_pages: list[tuple[int, int]] = []
    depths: list[int] = []

    def walk(page_id: int, depth: int) -> None:
        if page_id in seen:
            raise ValueError(f"index {owner}: page cycle/alias at {page_id}")
        seen.add(page_id)
        payload, page_type, _ = checked_page(image, page_id, page_size)
        depths.append(depth)
        if page_type == INDEX_INTERNAL:
            children = internal_children(payload, owner, page_count)
            internal_pages.append((page_id, len(children)))
            for child in children:
                walk(child, depth + 1)
        elif page_type == INDEX_LEAF:
            next_leaf, rows = leaf_entries(payload, owner)
            leaves.append((page_id, next_leaf, rows))
        else:
            raise ValueError(f"index {owner}: page {page_id} has unsupported page type {page_type}")

    walk(index["root"], 0)
    for position, (page_id, next_leaf, _) in enumerate(leaves):
        expected = leaves[position + 1][0] if position + 1 < len(leaves) else 0
        if next_leaf != expected:
            raise ValueError(f"index {owner}: leaf chain disagrees at page {page_id}")
    rows = [row for _, _, page_rows in leaves for row in page_rows]

    def physical(row: tuple[bytes, tuple[int, int]]) -> bytes:
        key, (page_id, slot) = row
        return key + page_id.to_bytes(8, "big") + slot.to_bytes(4, "big")

    if any(physical(left) >= physical(right) for left, right in zip(rows, rows[1:])):
        raise ValueError(f"index {owner}: physical keys are not strictly ordered")
    return {
        "rows": rows,
        "leaf_pages": [page_id for page_id, _, _ in leaves],
        "internal_pages": internal_pages,
        "depth": max(depths),
        "root_type": "internal" if internal_pages and internal_pages[0][0] == index["root"] else "leaf",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, default=Path(__file__).parent / "fixtures/NDB1R4P11R3AH/multilevel.gxdb")
    args = parser.parse_args()
    image = args.fixture.read_bytes()
    page_size = u32(image, 12)
    page_count = u64(image, 40)
    root_page = u64(image, 48)
    if page_size != PAGE_SIZE or len(image) != page_size * page_count:
        raise ValueError("fixture length or page size does not match the expected R3AH image")
    header = bytearray(image[:128])
    header_crc = u32(header, 64)
    header[64:68] = b"\0\0\0\0"
    if zlib.crc32(header) & 0xFFFFFFFF != header_crc:
        raise ValueError("database header checksum mismatch")

    tables, indexes, continuation_ids = parse_catalog(image, page_size, page_count, root_page)
    table_by_id = {table["id"]: table for table in tables}
    summaries = []
    boolean_groups: dict[str, list[list[int]]] = {}
    for index in indexes:
        table = table_by_id[index["table_id"]]
        columns = {column["ordinal"]: column for column in table["columns"]}
        column = columns[index["column_ordinal"]]
        key_type = column["type"]
        tree = index_tree(image, page_size, page_count, index)
        groups: dict[str, list[list[int]]] = {}
        for encoded_key, locator in tree["rows"]:
            decoded = decode_key(encoded_key, key_type)
            groups.setdefault(decoded, []).append(list(locator))
        if key_type == 1:
            boolean_groups = groups
        summaries.append({
            "id": index["id"],
            "name": index["name"],
            "table": table["name"],
            "table_id": index["table_id"],
            "column": column["name"],
            "column_ordinal": index["column_ordinal"],
            "key_type": TYPE_NAMES.get(key_type, str(key_type)),
            "root_page": index["root"],
            "root_type": tree["root_type"],
            "owner_index_id": index["id"],
            "recorded_entries": index["entry_count"],
            "observed_entries": len(tree["rows"]),
            "depth_edges": tree["depth"],
            "internal_pages": len(tree["internal_pages"]),
            "leaf_pages": len(tree["leaf_pages"]),
            "distinct_keys": len(groups),
            "duplicate_key_groups": {key: len(rows) for key, rows in groups.items() if len(rows) > 1},
            "null_entries": len(groups.get("NULL", [])),
            "first_key": decode_key(tree["rows"][0][0], key_type),
            "last_key": decode_key(tree["rows"][-1][0], key_type),
        })
    if len(indexes) != 6:
        raise ValueError(f"expected six persisted indexes, found {len(indexes)}")
    print(json.dumps({
        "fixture": str(args.fixture.resolve()),
        "sha256": hashlib.sha256(image).hexdigest(),
        "bytes": len(image),
        "page_size": page_size,
        "page_count": page_count,
        "catalog_root_page": root_page,
        "catalog_version": 3,
        "catalog_continuation_pages": continuation_ids,
        "table_count": len(tables),
        "index_count": len(indexes),
        "indexes": summaries,
        "boolean_expected_locators": {
            "NULL": boolean_groups.get("NULL", []),
            "false": boolean_groups.get("false", []),
            "true": boolean_groups.get("true", []),
        },
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
