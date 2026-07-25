# tools/CMakeLists.txt — Phase 1 transport diagnostics

foreach(tool rfcomm_ping rfcomm_bench raw_stream_sender raw_stream_receiver)
    add_executable(${tool} ${tool}.c)
    target_link_libraries(${tool} PRIVATE aether_transport)
endforeach()
