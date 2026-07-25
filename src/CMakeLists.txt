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
