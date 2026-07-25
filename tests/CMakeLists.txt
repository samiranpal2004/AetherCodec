# tests/CMakeLists.txt
#
# Test executables are registered here as their sources land:
#   test_packet   [done] Phase 1
#   test_lpc      Phase 2
#   test_mdct     Phase 3
#   test_jitter   Phase 5

# Keep assert() live even in Release builds — these tests assert their checks.
add_executable(test_packet test_packet.c)
target_link_libraries(test_packet PRIVATE aether_transport)
target_compile_options(test_packet PRIVATE -UNDEBUG)
add_test(NAME test_packet COMMAND test_packet)

add_executable(test_lpc test_lpc.c)
target_link_libraries(test_lpc PRIVATE aether_codec)
target_compile_options(test_lpc PRIVATE -UNDEBUG)
add_test(NAME test_lpc COMMAND test_lpc)

add_executable(test_codec_nl test_codec_nl.c)
target_link_libraries(test_codec_nl PRIVATE aether_codec)
target_compile_options(test_codec_nl PRIVATE -UNDEBUG)
add_test(NAME test_codec_nl COMMAND test_codec_nl)

add_executable(test_mdct test_mdct.c)
target_link_libraries(test_mdct PRIVATE aether_codec)
target_compile_options(test_mdct PRIVATE -UNDEBUG)
add_test(NAME test_mdct COMMAND test_mdct)

add_executable(test_codec_hq test_codec_hq.c)
target_link_libraries(test_codec_hq PRIVATE aether_codec)
target_compile_options(test_codec_hq PRIVATE -UNDEBUG)
add_test(NAME test_codec_hq COMMAND test_codec_hq)

add_executable(test_ring test_ring.c)
target_link_libraries(test_ring PRIVATE aether_runtime)
target_compile_options(test_ring PRIVATE -UNDEBUG)
add_test(NAME test_ring COMMAND test_ring)

add_executable(test_jitter test_jitter.c)
target_link_libraries(test_jitter PRIVATE aether_runtime)
target_compile_options(test_jitter PRIVATE -UNDEBUG)
add_test(NAME test_jitter COMMAND test_jitter)

add_executable(test_abr test_abr.c)
target_link_libraries(test_abr PRIVATE aether_runtime)
target_compile_options(test_abr PRIVATE -UNDEBUG)
add_test(NAME test_abr COMMAND test_abr)

add_executable(test_resample test_resample.c)
target_link_libraries(test_resample PRIVATE aether_runtime)
target_compile_options(test_resample PRIVATE -UNDEBUG)
add_test(NAME test_resample COMMAND test_resample)
