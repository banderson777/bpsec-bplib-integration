cmake_minimum_required(VERSION 3.16)
project(bpsec_bplib_adapter C)

set(BSL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../BSL" CACHE PATH "BSL checkout")
set(BPLIB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../bplib" CACHE PATH "BPLib checkout")
set(BSL_BUILD "${BSL_ROOT}/build/default" CACHE PATH "BSL build directory")
set(BPLIB_BUILD "${BPLIB_ROOT}/build" CACHE PATH "BPLib build directory")
# Use the staged package's include and library paths on clean checkouts.
find_package(qcbor 1.6 CONFIG REQUIRED HINTS "${BSL_ROOT}/testroot/usr")

add_executable(bpsec_bplib_smoke main.c bplib_bsl_adapter.c)
target_include_directories(bpsec_bplib_smoke PRIVATE
  "${BSL_ROOT}/src" "${BSL_BUILD}/src"
  "${BPLIB_ROOT}/inc" "${BPLIB_ROOT}/version/inc"
  "${BPLIB_ROOT}/ci/mem/inc" "${BPLIB_ROOT}/ci/eid/inc"
  "${BPLIB_ROOT}/ci/crc/inc" "${BPLIB_ROOT}/ci/time/inc"
  "${BPLIB_ROOT}/ci/rbt/inc" "${BPLIB_ROOT}/ci/qm/inc"
  "${BPLIB_ROOT}/bpa/ct/inc" "${BPLIB_ROOT}/aa/arp/inc")
target_include_directories(bpsec_bplib_smoke PRIVATE "${BSL_ROOT}/testroot/usr/include/m-lib")
target_link_directories(bpsec_bplib_smoke PRIVATE
  "${BSL_BUILD}/src" "${BPLIB_BUILD}" "${BPLIB_ROOT}/osal-staging/usr/lib"
  "${BSL_ROOT}/testroot/usr/lib/x86_64-linux-gnu")
if (WIN32)
  target_link_libraries(bpsec_bplib_smoke PRIVATE bsl_front bsl_dynamic bsl_mock_bpa qcbor::qcbor sqlite3)
else()
  target_link_libraries(bpsec_bplib_smoke PRIVATE
    bsl_front bsl_dynamic bsl_mock_bpa qcbor::qcbor sqlite3 m pthread dl rt)
endif()

find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
  add_test(NAME video_security_regression
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../client-test/test_security.py")
  set_tests_properties(video_security_regression PROPERTIES TIMEOUT 60 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
  if (NOT WIN32)
    add_test(NAME deployment_health_regression
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../demo/test_node_health.py")
    set_tests_properties(deployment_health_regression PROPERTIES TIMEOUT 10 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    # Exercise the stream's record and playlist contracts without FFmpeg or a display.
    add_test(NAME five_node_stream_regression
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../demo/test_five_node_stream.py")
    set_tests_properties(five_node_stream_regression PROPERTIES TIMEOUT 10 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
  endif()
endif()
set_target_properties(bpsec_bplib_smoke PROPERTIES BUILD_RPATH
  "${BSL_BUILD}/src;${BPLIB_BUILD};${BPLIB_ROOT}/osal-staging/usr/lib;${BSL_ROOT}/testroot/usr/lib/x86_64-linux-gnu")
target_compile_features(bpsec_bplib_smoke PRIVATE c_std_99)
target_compile_definitions(bpsec_bplib_smoke PRIVATE WIRE_FIXTURE=\"${BSL_ROOT}/test/fuzz_mock_bpa_bpv7_cbor-corpus/rfc9173-exA14.cbor\")
target_compile_options(bpsec_bplib_smoke PRIVATE -Wall -Wextra -Wpedantic)
enable_testing()
add_test(NAME bpsec_bplib_smoke COMMAND bpsec_bplib_smoke)

if (NOT WIN32)
add_executable(bplib_tcp_cla_smoke tcp_cla_smoke.c bplib_tcp_cla.c)
target_include_directories(bplib_tcp_cla_smoke PRIVATE
  "${BPLIB_ROOT}/inc" "${BPLIB_ROOT}/cla/inc"
  "${BPLIB_ROOT}/ci/eid/inc" "${BPLIB_ROOT}/ci/qm/inc"
  "${BPLIB_ROOT}/ci/mem/inc" "${BPLIB_ROOT}/ci/crc/inc"
  "${BPLIB_ROOT}/ci/time/inc" "${BPLIB_ROOT}/ci/rbt/inc"
  "${BPLIB_ROOT}/ci/em/inc"
  "${BPLIB_ROOT}/bpa/ct/inc"
  "${BPLIB_ROOT}/aa/arp/inc")
target_compile_features(bplib_tcp_cla_smoke PRIVATE c_std_99)
target_compile_options(bplib_tcp_cla_smoke PRIVATE -Wall -Wextra -Wpedantic)
if (WIN32)
  target_link_libraries(bplib_tcp_cla_smoke PRIVATE ws2_32)
else()
  target_link_libraries(bplib_tcp_cla_smoke PRIVATE pthread)
endif()
add_test(NAME bplib_tcp_cla_smoke COMMAND bplib_tcp_cla_smoke)

add_executable(tcpclv4_chain_smoke tcpclv4_chain_smoke.c bplib_tcp_cla.c)
target_include_directories(tcpclv4_chain_smoke PRIVATE
  "${BPLIB_ROOT}/inc" "${BPLIB_ROOT}/cla/inc"
  "${BPLIB_ROOT}/ci/eid/inc" "${BPLIB_ROOT}/ci/qm/inc"
  "${BPLIB_ROOT}/ci/mem/inc" "${BPLIB_ROOT}/ci/crc/inc"
  "${BPLIB_ROOT}/ci/time/inc" "${BPLIB_ROOT}/ci/rbt/inc"
  "${BPLIB_ROOT}/ci/em/inc" "${BPLIB_ROOT}/bpa/ct/inc"
  "${BPLIB_ROOT}/aa/arp/inc")
target_compile_features(tcpclv4_chain_smoke PRIVATE c_std_99)
target_compile_options(tcpclv4_chain_smoke PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tcpclv4_chain_smoke PRIVATE pthread)
add_test(NAME tcpclv4_chain_smoke COMMAND tcpclv4_chain_smoke)

add_executable(tcpclv4_stcp_chain_smoke tcpclv4_stcp_chain_smoke.c bplib_tcp_cla.c)
target_include_directories(tcpclv4_stcp_chain_smoke PRIVATE
  "${BPLIB_ROOT}/inc" "${BPLIB_ROOT}/cla/inc"
  "${BPLIB_ROOT}/ci/eid/inc" "${BPLIB_ROOT}/ci/qm/inc"
  "${BPLIB_ROOT}/ci/mem/inc" "${BPLIB_ROOT}/ci/crc/inc"
  "${BPLIB_ROOT}/ci/time/inc" "${BPLIB_ROOT}/ci/rbt/inc"
  "${BPLIB_ROOT}/ci/em/inc" "${BPLIB_ROOT}/bpa/ct/inc"
  "${BPLIB_ROOT}/aa/arp/inc")
target_compile_features(tcpclv4_stcp_chain_smoke PRIVATE c_std_99)
target_compile_options(tcpclv4_stcp_chain_smoke PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tcpclv4_stcp_chain_smoke PRIVATE pthread)
add_test(NAME tcpclv4_stcp_chain_smoke COMMAND tcpclv4_stcp_chain_smoke)

add_executable(tcpclv4_stcp_hdtn_chain_smoke tcpclv4_stcp_hdtn_chain_smoke.c bplib_tcp_cla.c)
target_include_directories(tcpclv4_stcp_hdtn_chain_smoke PRIVATE
  "${BPLIB_ROOT}/inc" "${BPLIB_ROOT}/cla/inc"
  "${BPLIB_ROOT}/ci/eid/inc" "${BPLIB_ROOT}/ci/qm/inc"
  "${BPLIB_ROOT}/ci/mem/inc" "${BPLIB_ROOT}/ci/crc/inc"
  "${BPLIB_ROOT}/ci/time/inc" "${BPLIB_ROOT}/ci/rbt/inc"
  "${BPLIB_ROOT}/ci/em/inc" "${BPLIB_ROOT}/bpa/ct/inc"
  "${BPLIB_ROOT}/aa/arp/inc")
target_compile_features(tcpclv4_stcp_hdtn_chain_smoke PRIVATE c_std_99)
target_compile_options(tcpclv4_stcp_hdtn_chain_smoke PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tcpclv4_stcp_hdtn_chain_smoke PRIVATE pthread)
add_test(NAME tcpclv4_stcp_hdtn_chain_smoke COMMAND tcpclv4_stcp_hdtn_chain_smoke)
endif()

# Compile the BPLib queue bridge as part of this integration project. The
# final executable supplies BPLib's platform tasking and callback dependencies.
add_library(bplib_tcp_cla OBJECT bplib_tcp_cla.c bplib_tcp_cla_bplib.c)
target_include_directories(bplib_tcp_cla PRIVATE
  "${BPLIB_ROOT}/inc" "${BPLIB_ROOT}/cla/inc"
  "${BPLIB_ROOT}/ci/eid/inc" "${BPLIB_ROOT}/ci/qm/inc"
  "${BPLIB_ROOT}/ci/mem/inc" "${BPLIB_ROOT}/ci/crc/inc"
  "${BPLIB_ROOT}/ci/time/inc" "${BPLIB_ROOT}/ci/rbt/inc"
  "${BPLIB_ROOT}/ci/em/inc" "${BPLIB_ROOT}/bpa/bi/inc"
  "${BPLIB_ROOT}/bpa/ct/inc" "${BPLIB_ROOT}/bpa/stor/inc"
  "${BPLIB_ROOT}/aa/arp/inc")
target_compile_features(bplib_tcp_cla PRIVATE c_std_99)
target_compile_options(bplib_tcp_cla PRIVATE -Wall -Wextra -Wpedantic)

if (NOT WIN32)
  add_executable(tcp_cla_regression tcp_cla_regression.c bplib_tcp_cla.c)
  add_executable(memory_regression memory_regression.c "${BPLIB_ROOT}/ci/mem/src/bplib_mem.c")
  foreach(target tcp_cla_regression memory_regression)
    target_include_directories(${target} PRIVATE $<TARGET_PROPERTY:bplib_tcp_cla,INCLUDE_DIRECTORIES>)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(${target} PRIVATE pthread)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 30)
  endforeach()
endif()

# Compile the actual BPNode task/sink against this checkout's generated cFS
# headers when available. Framework and decoder boundaries are stubbed in tests.
set(CFS_BUILD "${CMAKE_CURRENT_LIST_DIR}/../cfs-build" CACHE PATH "Configured cFS build for BPNode regressions")
set(BPNODE_FLAGS "${CFS_BUILD}/native/default/apps/bpnode/CMakeFiles/bpnode.dir/flags.make")
if (NOT WIN32 AND EXISTS "${BPNODE_FLAGS}")
  file(STRINGS "${BPNODE_FLAGS}" BPNODE_INCLUDES REGEX "^C_INCLUDES =")
  string(REPLACE "C_INCLUDES = " "" BPNODE_INCLUDES "${BPNODE_INCLUDES}")
  separate_arguments(BPNODE_INCLUDES UNIX_COMMAND "${BPNODE_INCLUDES}")
  add_executable(bpnode_ingress_regression bpnode_ingress_regression.c ../bpnode/fsw/src/bpnode_cla_in.c)
  add_executable(video_sink_regression video_sink_regression.c ../bpnode/fsw/src/bpnode_video_sink.c)
  add_executable(demo_delivery_regression demo_delivery_regression.c)
  target_link_options(demo_delivery_regression PRIVATE -Wl,--wrap=fsync -Wl,--wrap=fopen)
  target_include_directories(demo_delivery_regression PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${BPLIB_ROOT}/bpa/ct/src")
  add_executable(custody_db_regression custody_db_regression.c
    "${BPLIB_ROOT}/bpa/ct/src/bplib_ct_db.c"
    "${BPLIB_ROOT}/ci/rbt/src/bplib_rbt_api.c"
    "${BPLIB_ROOT}/ci/rbt/src/bplib_rbt_impl.c")
  target_include_directories(custody_db_regression PRIVATE "${BPLIB_ROOT}/bpa/ct/src" "${BPLIB_ROOT}/ci/rbt/src")
  add_executable(custody_admission_regression custody_admission_regression.c
    "${BPLIB_ROOT}/bpa/ct/src/bplib_ct.c" "${BPLIB_ROOT}/bpa/ct/src/bplib_ct_db.c"
    "${BPLIB_ROOT}/ci/rbt/src/bplib_rbt_api.c" "${BPLIB_ROOT}/ci/rbt/src/bplib_rbt_impl.c")
  target_include_directories(custody_admission_regression PRIVATE "${BPLIB_ROOT}/bpa/ct/src" "${BPLIB_ROOT}/ci/rbt/src")
  add_executable(custody_regression custody_regression.c "${BPLIB_ROOT}/bpa/ct/src/bplib_ct.c")
  target_include_directories(custody_regression PRIVATE "${BPLIB_ROOT}/bpa/ct/src")
  target_compile_definitions(video_sink_regression PRIVATE BPNODE_VIDEO_SINK_ENABLE)
  foreach(target bpnode_ingress_regression video_sink_regression custody_regression custody_db_regression custody_admission_regression demo_delivery_regression)
    target_compile_options(${target} PRIVATE ${BPNODE_INCLUDES} -ffunction-sections -fdata-sections)
    target_link_options(${target} PRIVATE -Wl,--gc-sections)
    target_link_libraries(${target} PRIVATE pthread)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 10)
  endforeach()
endif()

# Decode the extension-array boundary using the real decoder and QCBOR.
if (NOT WIN32)
  add_executable(cbor_bounds_regression cbor_bounds_regression.c
    "${BPLIB_ROOT}/ci/cbor/src/bplib_cbor_decode_canonical.c")
  target_include_directories(cbor_bounds_regression PRIVATE
    $<TARGET_PROPERTY:bplib_tcp_cla,INCLUDE_DIRECTORIES>
    "${BPLIB_ROOT}/ci/cbor/src" "${BPLIB_ROOT}/ci/cbor/inc"
    "${BPLIB_ROOT}/ci/pl/inc" "${BPLIB_ROOT}/version/inc" "${BPLIB_ROOT}/aa/nc/inc"
    "${BPLIB_ROOT}/aa/as/inc" "${BPLIB_ROOT}/aa/fwp/inc"
    "${BPLIB_ROOT}/bpa/pi/inc" "${BPLIB_ROOT}/bpa/ebp/inc"
    "${BPLIB_ROOT}/bpa/pdb/inc" "${BSL_ROOT}/testroot/usr/include")
  target_link_directories(cbor_bounds_regression PRIVATE "${BSL_ROOT}/testroot/usr/lib/x86_64-linux-gnu")
  target_link_libraries(cbor_bounds_regression PRIVATE qcbor::qcbor)
  target_compile_options(cbor_bounds_regression PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME cbor_bounds_regression COMMAND cbor_bounds_regression)
  set_tests_properties(cbor_bounds_regression PROPERTIES TIMEOUT 10)
endif()

if (NOT WIN32)
  add_executable(storage_init_regression storage_init_regression.c
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql.c")
  target_include_directories(storage_init_regression PRIVATE
    $<TARGET_PROPERTY:cbor_bounds_regression,INCLUDE_DIRECTORIES>)
  target_compile_options(storage_init_regression PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
  target_link_options(storage_init_regression PRIVATE -Wl,--gc-sections)
  target_link_libraries(storage_init_regression PRIVATE sqlite3 pthread)
  add_test(NAME storage_init_regression COMMAND storage_init_regression)
  set_tests_properties(storage_init_regression PROPERTIES TIMEOUT 10)
endif()

if (NOT WIN32)
  add_executable(storage_commit_regression storage_commit_regression.c
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql_store.c")
  target_include_directories(storage_commit_regression PRIVATE
    $<TARGET_PROPERTY:cbor_bounds_regression,INCLUDE_DIRECTORIES>
    "${BPLIB_ROOT}/bpa/stor/src" "${BPLIB_ROOT}/bpa/ct/src")
  target_compile_definitions(storage_commit_regression PRIVATE BPLIB_STOR_INSERTBATCHSIZE=2)
  target_compile_options(storage_commit_regression PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
  target_link_options(storage_commit_regression PRIVATE -Wl,--gc-sections)
  target_link_libraries(storage_commit_regression PRIVATE sqlite3 pthread)
  add_test(NAME storage_commit_regression COMMAND storage_commit_regression)
  set_tests_properties(storage_commit_regression PROPERTIES TIMEOUT 10)
endif()

if (NOT WIN32)
  add_executable(sql_ownership_regression sql_ownership_regression.c
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql_load.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_loadbatch.c")
  target_include_directories(sql_ownership_regression PRIVATE
    $<TARGET_PROPERTY:storage_commit_regression,INCLUDE_DIRECTORIES>)
  target_compile_options(sql_ownership_regression PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
  target_link_options(sql_ownership_regression PRIVATE -Wl,--gc-sections)
  target_link_libraries(sql_ownership_regression PRIVATE sqlite3 pthread)
  add_test(NAME sql_ownership_regression COMMAND sql_ownership_regression)
  set_tests_properties(sql_ownership_regression PROPERTIES TIMEOUT 20)
endif()

if (NOT WIN32)
  add_executable(custody_parallel_regression custody_parallel_regression.c
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql_cust.c")
  target_include_directories(custody_parallel_regression PRIVATE
    $<TARGET_PROPERTY:storage_commit_regression,INCLUDE_DIRECTORIES>)
  target_compile_options(custody_parallel_regression PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
  target_link_options(custody_parallel_regression PRIVATE -Wl,--gc-sections)
  target_link_libraries(custody_parallel_regression PRIVATE sqlite3 pthread)
  add_test(NAME custody_parallel_regression COMMAND custody_parallel_regression)
  set_tests_properties(custody_parallel_regression PROPERTIES TIMEOUT 20)
endif()

if (NOT WIN32)
  add_executable(storage_binding_regression storage_binding_regression.c
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql_store.c")
  target_include_directories(storage_binding_regression PRIVATE
    $<TARGET_PROPERTY:storage_commit_regression,INCLUDE_DIRECTORIES>)
  target_compile_options(storage_binding_regression PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
  target_link_options(storage_binding_regression PRIVATE -Wl,--gc-sections)
  target_link_libraries(storage_binding_regression PRIVATE sqlite3)
  add_test(NAME storage_binding_regression COMMAND storage_binding_regression)
  set_tests_properties(storage_binding_regression PROPERTIES TIMEOUT 10)
endif()

if (NOT WIN32)
  add_executable(storage_parallel_regression storage_parallel_regression.c
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql_store.c")
  target_include_directories(storage_parallel_regression PRIVATE
    $<TARGET_PROPERTY:storage_commit_regression,INCLUDE_DIRECTORIES>)
  target_compile_options(storage_parallel_regression PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
  target_link_options(storage_parallel_regression PRIVATE -Wl,--gc-sections)
  target_link_libraries(storage_parallel_regression PRIVATE sqlite3 pthread)
  add_test(NAME storage_parallel_regression COMMAND storage_parallel_regression)
  set_tests_properties(storage_parallel_regression PROPERTIES TIMEOUT 20)
endif()

if (NOT WIN32)
  add_executable(storage_error_regression storage_error_regression.c
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_loadbatch.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql_load.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql_cust.c"
    "${BPLIB_ROOT}/bpa/ct/src/bplib_ct_ccs.c")
  target_include_directories(storage_error_regression PRIVATE
    $<TARGET_PROPERTY:cbor_bounds_regression,INCLUDE_DIRECTORIES>
    "${BPLIB_ROOT}/bpa/stor/src" "${BPLIB_ROOT}/bpa/ct/src")
  target_compile_options(storage_error_regression PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
  target_link_options(storage_error_regression PRIVATE -Wl,--gc-sections -Wl,--wrap=malloc)
  target_link_libraries(storage_error_regression PRIVATE sqlite3 pthread)
  foreach(mode discard_expired discard_egressed mark_egress custody_update load custody_queue lookup ccs_ranges)
    add_test(NAME storage_error_${mode}_regression COMMAND storage_error_regression ${mode})
    set_tests_properties(storage_error_${mode}_regression PROPERTIES TIMEOUT 10)
  endforeach()
endif()

# Endpoint wire security, shared by host tools and the optional cFS demo.
if (NOT WIN32)
  add_library(bplib_bsl_security SHARED bplib_bsl_security.c)
  target_include_directories(bplib_bsl_security PRIVATE $<TARGET_PROPERTY:bpsec_bplib_smoke,INCLUDE_DIRECTORIES>)
  target_link_directories(bplib_bsl_security PRIVATE "${BSL_BUILD}/src" "${BSL_ROOT}/testroot/usr/lib/x86_64-linux-gnu")
  target_link_libraries(bplib_bsl_security PRIVATE bsl_mock_bpa bsl_default_sc bsl_crypto bsl_dynamic bsl_front qcbor::qcbor pthread)
  target_compile_options(bplib_bsl_security PRIVATE -Wall -Wextra)
  set_target_properties(bplib_bsl_security PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES BUILD_RPATH "${BSL_BUILD}/src;${BSL_ROOT}/testroot/usr/lib/x86_64-linux-gnu")
  if (Python3_Interpreter_FOUND)
    add_test(NAME endpoint_security_regression
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../demo/test_security.py" "$<TARGET_FILE:bplib_bsl_security>")
    set_tests_properties(endpoint_security_regression PROPERTIES TIMEOUT 60 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
  endif()
endif()

if (NOT WIN32)
  add_executable(storage_recovery_regression storage_recovery_regression.c
    "${BPLIB_ROOT}/bpa/ct/src/bplib_ct_ccs.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql_store.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql_load.c"
    "${BPLIB_ROOT}/bpa/stor/src/bplib_stor_sql_cust.c"
    "${BPLIB_ROOT}/ci/mem/src/bplib_mem.c"
    "${BPLIB_ROOT}/ci/eid/src/bplib_eid.c"
    "${BPLIB_ROOT}/ci/crc/src/bplib_crc.c"
    "${BPLIB_ROOT}/ci/cbor/src/bplib_cbor_decode_bundle.c"
    "${BPLIB_ROOT}/ci/cbor/src/bplib_cbor_decode_primary.c"
    "${BPLIB_ROOT}/ci/cbor/src/bplib_cbor_decode_canonical.c"
    "${BPLIB_ROOT}/ci/cbor/src/bplib_cbor_decode_types.c"
    "${BPLIB_ROOT}/ci/cbor/src/bplib_cbor_crc.c"
    "${BPLIB_ROOT}/ci/cbor/src/bplib_cbor_encode_bundle.c"
    "${BPLIB_ROOT}/ci/cbor/src/bplib_cbor_encode_primary.c"
    "${BPLIB_ROOT}/ci/cbor/src/bplib_cbor_encode_extension.c"
    "${BPLIB_ROOT}/ci/cbor/src/bplib_cbor_encode_payload.c"
    "${BPLIB_ROOT}/ci/cbor/src/bplib_cbor_encode_types.c")
  target_include_directories(storage_recovery_regression PRIVATE
    $<TARGET_PROPERTY:cbor_bounds_regression,INCLUDE_DIRECTORIES>
    "${BPLIB_ROOT}/bpa/stor/src" "${BPLIB_ROOT}/bpa/ct/src" "${BPLIB_ROOT}/ci/crc/src")
  target_compile_options(storage_recovery_regression PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
  target_link_options(storage_recovery_regression PRIVATE -Wl,--gc-sections)
  target_link_libraries(storage_recovery_regression PRIVATE sqlite3 pthread qcbor::qcbor)
  target_link_directories(storage_recovery_regression PRIVATE "${BSL_ROOT}/testroot/usr/lib/x86_64-linux-gnu")
  foreach(mode recovery durability constructed ccs_parse ccs_sender)
    add_test(NAME storage_${mode}_regression COMMAND storage_recovery_regression ${mode})
    set_tests_properties(storage_${mode}_regression PROPERTIES TIMEOUT 20)
  endforeach()
endif()

if (NOT WIN32)
  add_executable(crc_regression crc_regression.c "${BPLIB_ROOT}/ci/crc/src/bplib_crc.c")
  target_include_directories(crc_regression PRIVATE $<TARGET_PROPERTY:bplib_tcp_cla,INCLUDE_DIRECTORIES>)
  target_compile_options(crc_regression PRIVATE -Wall -Wextra -Wpedantic -Werror=cast-function-type)
  add_test(NAME crc_regression COMMAND crc_regression)
  set_tests_properties(crc_regression PROPERTIES TIMEOUT 20)
endif()
