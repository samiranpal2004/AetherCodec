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
