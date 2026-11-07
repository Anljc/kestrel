cmake_minimum_required(VERSION 3.20)
project(kestrel VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 统一输出目录，避免 Debug/Release 各回各处找不到产物。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(KESTREL_WARNINGS "")
if(MSVC)
  # /utf-8：本仓库源码是 UTF-8，缺了它在 GBK 代码页下会报 C4819，中文字符串也可能乱码。
  list(APPEND KESTREL_WARNINGS /W4 /permissive- /Zc:__cplusplus /utf-8)
  # fopen 等 CRT 函数在 MSVC 下被标记为 deprecated（C4996），这里关掉该提示。
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  list(APPEND KESTREL_WARNINGS -Wall -Wextra -Wpedantic -Werror -Wshadow)
endif()

add_library(kestrel_core STATIC
  src/util/coding.cpp
  src/util/crc32c.cpp
  src/util/env.cpp
  src/util/logger.cpp
  src/util/comparator.cpp
  src/wal/wal_writer.cpp
  src/wal/wal_reader.cpp
  src/memtable/skiplist.cpp
  src/memtable/memtable.cpp
  src/db/dbformat.cpp
  src/db/db_impl.cpp
  src/db/version.cpp
  src/cache/lru_cache.cpp
  src/compaction/compactor.cpp
  src/sstable/block.cpp
  src/sstable/format.cpp
  src/sstable/bloom_filter.cpp
  src/sstable/table_builder.cpp
  src/sstable/table_reader.cpp
)
# src 也设为 PUBLIC：测试和工具需要直接 include 内部头文件（util/coding.h 等）。
target_include_directories(kestrel_core PUBLIC include src)
target_compile_options(kestrel_core PRIVATE ${KESTREL_WARNINGS})

enable_testing()

function(kestrel_add_test name)
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} PRIVATE kestrel_core)
  target_include_directories(${name} PRIVATE tests)
  target_compile_options(${name} PRIVATE ${KESTREL_WARNINGS})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

kestrel_add_test(test_coding)
kestrel_add_test(test_env)
kestrel_add_test(test_wal)
kestrel_add_test(test_skiplist)
kestrel_add_test(test_memtable)
kestrel_add_test(test_db)
kestrel_add_test(test_sstable)
kestrel_add_test(test_recovery)

# ---- 工具：故障注入与基准 ----
# 工具不属于测试，但共用 kestrel_core；用同一个告警档位，避免"工具里随便写"。
function(kestrel_add_tool name)
  add_executable(${name} tools/${name}.cpp)
  target_link_libraries(${name} PRIVATE kestrel_core)
  target_compile_options(${name} PRIVATE ${KESTREL_WARNINGS})
endfunction()

kestrel_add_tool(crash_test)
kestrel_add_tool(db_bench)

# 强杀测试要真的拉起子进程再 SIGKILL，天然带时序，给足超时但轮次不宜多，
# 否则 CTest 单次运行时间会被 I/O 抖动放大。
add_test(NAME crash_test COMMAND crash_test --rounds 6 --dir ${CMAKE_BINARY_DIR}/crash_db
                                             --min_delay_ms 40 --max_delay_ms 120)
set_tests_properties(crash_test PROPERTIES TIMEOUT 180)

# 截断/位翻转模式在 CTest 里也跑一遍，保证故障注入本身没失效。
add_test(NAME crash_test_truncate COMMAND crash_test --mode truncate
                                                    --dir ${CMAKE_BINARY_DIR}/crash_trunc_db)
add_test(NAME crash_test_bitflip COMMAND crash_test --mode bitflip
                                                   --dir ${CMAKE_BINARY_DIR}/crash_flip_db)
