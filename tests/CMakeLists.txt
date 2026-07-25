# tests/CMakeLists.txt
#
# Test executables are registered here as their sources land:
#   test_packet   [done] Phase 1
#   test_lpc      Phase 2
#   test_mdct     Phase 3
#   test_jitter   Phase 5

add_executable(test_packet test_packet.c)
target_link_libraries(test_packet PRIVATE aether_transport)
# Keep assert() live even in Release builds — these tests assert their checks.
target_compile_options(test_packet PRIVATE -UNDEBUG)
add_test(NAME test_packet COMMAND test_packet)
