cmake_minimum_required(VERSION 3.16)

project(guideXOSSqlDatabase LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(gxos_database STATIC
    database/database_format.cpp
    database/database_checksum.cpp
    database/database_io.cpp
    database/database_header.cpp
    database/database_page.cpp
    database/database_file.cpp
    database/database_engine.cpp
    database/database_buffer.cpp
    database/database_catalog.cpp
    database/database_heap.cpp
    database/database_relational.cpp
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

add_executable(gxdb_cli tools/gxdb_cli.cpp)
target_link_libraries(gxdb_cli PRIVATE gxos_database)
if(MSVC)
    target_compile_options(gxdb_cli PRIVATE /W4)
else()
    target_compile_options(gxdb_cli PRIVATE -Wall -Wextra)
endif()
