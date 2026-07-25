# src/CMakeLists.txt
#
# Targets are added here as each phase of the implementation plan lands.
#
#   Phase 1  [done] transport/aether_packet.c, transport/transport_rfcomm.c
#   Phase 2  encoder/codec_lpc_enc.c, decoder/codec_lpc_dec.c, encoder/aether_encoder.c
#   Phase 3  encoder/codec_mdct_enc.c, decoder/codec_mdct_dec.c
#   Phase 4  os/pw_sink.c, os/pw_source.c, os/alsa_plugin.c, daemon/*.c
#   Phase 5  abr/abr_ctrl.c, jitter/jitter_buf.c

# --- Phase 1: transport + packet layer ---
add_library(aether_transport STATIC
    transport/aether_packet.c
    transport/transport_rfcomm.c
)
target_include_directories(aether_transport PUBLIC ${CMAKE_SOURCE_DIR}/include)
# libbluetooth (from libbluetooth-dev) provides ba2str/str2ba and the RFCOMM API.
target_link_libraries(aether_transport PUBLIC ${BLUEZ_LIBRARIES} bluetooth)

# --- Phase 2: Near-Lossless codec (LPC + Rice) ---
# --- Phase 3 adds the MDCT / perceptual HQ sources to the same library ---
add_library(aether_codec STATIC
    encoder/codec_lpc_enc.c
    decoder/codec_lpc_dec.c
    encoder/codec_mdct_enc.c
    decoder/codec_mdct_dec.c
    encoder/aether_encoder.c
    decoder/aether_decoder.c
)
target_include_directories(aether_codec PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_directories(aether_codec PUBLIC ${FFTW_LIBRARY_DIRS})
# aether_transport supplies aether_timestamp_us/packet helpers; m for log2/fabs;
# fftw3f (single precision) provides the DCT-IV behind the MDCT.
target_link_libraries(aether_codec PUBLIC aether_transport ${FFTW_LIBRARIES} m)

# --- Phase 4: runtime plumbing (ring buffer + jitter buffer) ---
# No PipeWire dependency here, so these build and are testable regardless of
# whether AETHER_ENABLE_PIPEWIRE is on.
add_library(aether_runtime STATIC
    os/audio_ring.c
    jitter/jitter_buf.c
)
target_include_directories(aether_runtime PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(aether_runtime PUBLIC aether_transport Threads::Threads)

# --- Phase 4: PipeWire OS integration + daemons ---
if(AETHER_ENABLE_PIPEWIRE)
    add_library(aether_os STATIC
        os/pw_sink.c
        os/pw_playback.c
    )
    target_include_directories(aether_os PUBLIC
        ${CMAKE_SOURCE_DIR}/include ${PIPEWIRE_INCLUDE_DIRS})
    target_link_directories(aether_os PUBLIC ${PIPEWIRE_LIBRARY_DIRS})
    target_link_libraries(aether_os PUBLIC aether_runtime ${PIPEWIRE_LIBRARIES})

    add_executable(aether_sender   daemon/aether_sender.c)
    add_executable(aether_receiver daemon/aether_receiver.c)
    foreach(d aether_sender aether_receiver)
        target_link_libraries(${d} PRIVATE aether_os aether_codec)
    endforeach()
endif()
