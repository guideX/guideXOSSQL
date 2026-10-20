cmake_minimum_required(VERSION 3.16)

project(guideXOSSqlDatabase LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(gxos_database STATIC
    database/database_format.cpp
    database/database_checksum.cpp
    database/database_io.cpp
    database/database_native_io.cpp
    database/database_header.cpp
    database/database_page.cpp
    database/database_file.cpp
    database/database_engine.cpp
    database/database_buffer.cpp
    database/database_catalog.cpp
    database/database_index.cpp
    database/database_heap.cpp
    database/database_wal.cpp
    database/portable/gx_allocator.cpp
    database/portable/gx_allocator_hosted.cpp
    database/portable/gx_array.cpp
    database/portable/gx_owned_page.cpp
    database/portable/gx_heap_page_builder.cpp
    database/portable/gx_recovered_pages.cpp
    database/portable/gx_page_cache.cpp
    database/portable/gx_page_codec.cpp
    database/portable/gx_wal_record.cpp
    database/portable/gx_wal_scan.cpp
    database/portable/gx_wal_page_image.cpp
    database/portable/gx_transaction_pages.cpp
    database/portable/gx_transaction_page_index.cpp
    database/portable/gx_transaction_page_overlay.cpp
    database/database_transaction.cpp
    database/database_relational.cpp
    database/database_sql_token.cpp
    database/database_sql_tokenizer.cpp
    database/database_sql_parser.cpp
    database/database_sql_expr.cpp
    database/database_sql.cpp
)
target_include_directories(gxos_database PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/database")

if(MSVC)
    target_compile_options(gxos_database PRIVATE /W4)
else()
    target_compile_options(gxos_database PRIVATE -Wall -Wextra)
endif()

enable_testing()

add_executable(database_storage_tests tests/database_storage_test.cpp)
target_link_libraries(database_storage_tests PRIVATE gxos_database)
if(MSVC)
    target_compile_options(database_storage_tests PRIVATE /W4)
else()
    target_compile_options(database_storage_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME database_storage_tests COMMAND database_storage_tests)

add_executable(database_relational_test tests/database_relational_test.cpp)
target_link_libraries(database_relational_test PRIVATE gxos_database)
if(MSVC)
    target_compile_options(database_relational_test PRIVATE /W4)
else()
    target_compile_options(database_relational_test PRIVATE -Wall -Wextra)
endif()

add_test(NAME database_relational_test COMMAND database_relational_test)

add_executable(database_transaction_test tests/database_transaction_test.cpp)
target_link_libraries(database_transaction_test PRIVATE gxos_database)
if(MSVC)
    target_compile_options(database_transaction_test PRIVATE /W4)
else()
    target_compile_options(database_transaction_test PRIVATE -Wall -Wextra)
endif()

add_test(NAME database_transaction_test COMMAND database_transaction_test)

add_executable(database_sql_test tests/database_sql_test.cpp)
target_link_libraries(database_sql_test PRIVATE gxos_database)
if(MSVC)
    target_compile_options(database_sql_test PRIVATE /W4)
else()
    target_compile_options(database_sql_test PRIVATE -Wall -Wextra)
endif()

add_test(NAME database_sql_test COMMAND database_sql_test)

add_executable(database_index_test tests/database_index_test.cpp)
target_link_libraries(database_index_test PRIVATE gxos_database)
if(MSVC)
    target_compile_options(database_index_test PRIVATE /W4)
else()
    target_compile_options(database_index_test PRIVATE -Wall -Wextra)
endif()

add_test(NAME database_index_test COMMAND database_index_test)

add_executable(database_sql7_test tests/database_sql7_test.cpp)
target_link_libraries(database_sql7_test PRIVATE gxos_database)
if(MSVC)
    target_compile_options(database_sql7_test PRIVATE /W4)
else()
    target_compile_options(database_sql7_test PRIVATE -Wall -Wextra)
endif()

add_test(NAME database_sql7_test COMMAND database_sql7_test)

add_executable(database_sql8_test tests/database_sql8_test.cpp)
target_link_libraries(database_sql8_test PRIVATE gxos_database)
if(MSVC)
    target_compile_options(database_sql8_test PRIVATE /W4)
else()
    target_compile_options(database_sql8_test PRIVATE -Wall -Wextra)
endif()

add_test(NAME database_sql8_test COMMAND database_sql8_test)

add_executable(database_sql9_test tests/database_sql9_test.cpp)
target_link_libraries(database_sql9_test PRIVATE gxos_database)
if(MSVC)
    target_compile_options(database_sql9_test PRIVATE /W4)
else()
    target_compile_options(database_sql9_test PRIVATE -Wall -Wextra)
endif()

add_test(NAME database_sql9_test COMMAND database_sql9_test)

add_executable(database_sql9r_test tests/database_sql9r_test.cpp)
target_link_libraries(database_sql9r_test PRIVATE gxos_database)
if(MSVC)
    target_compile_options(database_sql9r_test PRIVATE /W4)
else()
    target_compile_options(database_sql9r_test PRIVATE -Wall -Wextra)
endif()

add_test(NAME database_sql9r_test COMMAND database_sql9r_test)

add_executable(database_native_io_test tests/database_native_io_test.cpp)
target_link_libraries(database_native_io_test PRIVATE gxos_database)
if(MSVC)
    target_compile_options(database_native_io_test PRIVATE /W4)
else()
    target_compile_options(database_native_io_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME database_native_io_test COMMAND database_native_io_test)

# Keep the portable-runtime primitives independently buildable from the SQL
# engine; the reference model and fault-injection allocator live in the test.
add_executable(database_portable_runtime_test
    database/portable/gx_allocator.cpp
    database/portable/gx_allocator_hosted.cpp
    database/portable/gx_array.cpp
    database/portable/gx_owner.cpp
    database/portable/gx_string.cpp
    tests/gx_test_allocator.cpp
    tests/database_portable_runtime_test.cpp
)
target_include_directories(database_portable_runtime_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/database/portable"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests"
)
if(MSVC)
    target_compile_options(database_portable_runtime_test PRIVATE /W4)
else()
    target_compile_options(database_portable_runtime_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME database_portable_runtime_test COMMAND database_portable_runtime_test)

add_executable(database_wal_portable_test
    tests/gx_test_allocator.cpp
    tests/database_wal_portable_test.cpp
)
target_link_libraries(database_wal_portable_test PRIVATE gxos_database)
target_include_directories(database_wal_portable_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/database/portable"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests"
)
if(MSVC)
    target_compile_options(database_wal_portable_test PRIVATE /W4)
else()
    target_compile_options(database_wal_portable_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME database_wal_portable_test COMMAND database_wal_portable_test)

add_executable(database_wal_page_portable_test
    tests/gx_test_allocator.cpp
    tests/database_wal_page_portable_test.cpp
)
target_link_libraries(database_wal_page_portable_test PRIVATE gxos_database)
target_include_directories(database_wal_page_portable_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/database/portable"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests"
)
if(MSVC)
    target_compile_options(database_wal_page_portable_test PRIVATE /W4)
else()
    target_compile_options(database_wal_page_portable_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME database_wal_page_portable_test COMMAND database_wal_page_portable_test)

add_executable(database_wal_recovery_portable_test
    tests/gx_test_allocator.cpp
    tests/database_wal_recovery_portable_test.cpp
)
target_link_libraries(database_wal_recovery_portable_test PRIVATE gxos_database)
target_include_directories(database_wal_recovery_portable_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/database/portable"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests"
)
if(MSVC)
    target_compile_options(database_wal_recovery_portable_test PRIVATE /W4)
else()
    target_compile_options(database_wal_recovery_portable_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME database_wal_recovery_portable_test COMMAND database_wal_recovery_portable_test)

add_executable(database_ndb1r4p5_test
    tests/gx_test_allocator.cpp
    tests/database_ndb1r4p5_test.cpp
)
target_link_libraries(database_ndb1r4p5_test PRIVATE gxos_database)
target_include_directories(database_ndb1r4p5_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/database/portable"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests"
)
if(MSVC)
    target_compile_options(database_ndb1r4p5_test PRIVATE /W4)
else()
    target_compile_options(database_ndb1r4p5_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME database_ndb1r4p5_test COMMAND database_ndb1r4p5_test)

add_executable(database_ndb1r4p6_test
    tests/gx_test_allocator.cpp
    tests/database_ndb1r4p6_test.cpp
)
target_link_libraries(database_ndb1r4p6_test PRIVATE gxos_database)
target_include_directories(database_ndb1r4p6_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/database/portable"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests"
)
if(MSVC)
    target_compile_options(database_ndb1r4p6_test PRIVATE /W4)
else()
    target_compile_options(database_ndb1r4p6_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME database_ndb1r4p6_test COMMAND database_ndb1r4p6_test)

add_executable(database_ndb1r4p7_test
    tests/database_ndb1r4p7_test.cpp
)
target_link_libraries(database_ndb1r4p7_test PRIVATE gxos_database)
target_include_directories(database_ndb1r4p7_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/database/portable"
)
if(MSVC)
    target_compile_options(database_ndb1r4p7_test PRIVATE /W4)
else()
    target_compile_options(database_ndb1r4p7_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME database_ndb1r4p7_test COMMAND database_ndb1r4p7_test)

add_executable(database_ndb1r4p8_test
    tests/database_ndb1r4p8_test.cpp
)
target_link_libraries(database_ndb1r4p8_test PRIVATE gxos_database)
target_include_directories(database_ndb1r4p8_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/database/portable"
)
if(MSVC)
    target_compile_options(database_ndb1r4p8_test PRIVATE /W4)
else()
    target_compile_options(database_ndb1r4p8_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME database_ndb1r4p8_test COMMAND database_ndb1r4p8_test)

add_executable(database_heap_page_construction_test
    tests/database_heap_page_construction_test.cpp
)
target_link_libraries(database_heap_page_construction_test PRIVATE gxos_database)
target_include_directories(database_heap_page_construction_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/database/portable"
)
if(MSVC)
    target_compile_options(database_heap_page_construction_test PRIVATE /W4)
else()
    target_compile_options(database_heap_page_construction_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME database_heap_page_construction_test
    COMMAND database_heap_page_construction_test)

add_executable(database_owned_page_test
    tests/gx_test_allocator.cpp
    tests/database_owned_page_test.cpp
)
target_link_libraries(database_owned_page_test PRIVATE gxos_database)
target_include_directories(database_owned_page_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/database/portable"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests"
)
if(MSVC)
    target_compile_options(database_owned_page_test PRIVATE /W4)
else()
    target_compile_options(database_owned_page_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME database_owned_page_test COMMAND database_owned_page_test)

option(GX_ENABLE_PORTABLE_SANITIZERS "Enable ASan and UBSan for the portable runtime test" OFF)
if(GX_ENABLE_PORTABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(database_portable_runtime_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(database_portable_runtime_test PRIVATE
        -fsanitize=address,undefined)
endif()

add_executable(gxdb_cli tools/gxdb_cli.cpp)
target_link_libraries(gxdb_cli PRIVATE gxos_database)
if(MSVC)
    target_compile_options(gxdb_cli PRIVATE /W4)
else()
    target_compile_options(gxdb_cli PRIVATE -Wall -Wextra)
endif()
