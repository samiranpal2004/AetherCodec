# tools/CMakeLists.txt — Phase 1 transport diagnostics + Phase 6 file tools

foreach(tool rfcomm_ping rfcomm_bench raw_stream_sender raw_stream_receiver)
    add_executable(${tool} ${tool}.c)
    target_link_libraries(${tool} PRIVATE aether_transport)
endforeach()

# --- Phase 6: file-to-file encode/decode for single-machine measurement ---
# Needs only aether_codec (+ its transitive aether_transport) — no PipeWire,
# no Bluetooth link, so these build and run regardless of AETHER_ENABLE_PIPEWIRE.
foreach(tool aether_encode aether_decode)
    add_executable(${tool} ${tool}.c wav_io.c)
    target_link_libraries(${tool} PRIVATE aether_codec)
endforeach()
