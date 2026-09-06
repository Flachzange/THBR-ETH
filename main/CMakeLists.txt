# Exactly one backbone transport is compiled (see main/backbone.h).
set(THBR_EXTRA_SRC)

if(CONFIG_THBR_TRANSPORT_TAP)
    set(THBR_TRANSPORT_SRC "eth_link.c")
    list(APPEND THBR_EXTRA_SRC "p2p_netif_glue.c")
elseif(CONFIG_THBR_TRANSPORT_PPP)
    set(THBR_TRANSPORT_SRC "ppp_link.c")
    list(APPEND THBR_EXTRA_SRC "p2p_netif_glue.c")
elseif(CONFIG_THBR_TRANSPORT_W5500)
    set(THBR_TRANSPORT_SRC "w5500_link.c")
else()
    # During ESP-IDF's early requirements pass, project Kconfig values are
    # not available yet. Use a harmless fallback for that pass.
    set(THBR_TRANSPORT_SRC "ppp_link.c")
    list(APPEND THBR_EXTRA_SRC "p2p_netif_glue.c")
endif()

# ESP-IDF 6.0.2 ships OTBR as a prebuilt library. Its lwIP struct netif ABI
# assumes PPP is disabled; PPP adds a client-data pointer before netif->num and
# makes OTBR mistake Ethernet's hwaddr_len (6) for interface number 6 (index 7).
if(CONFIG_THBR_TRANSPORT_W5500 AND CONFIG_LWIP_PPP_SUPPORT)
    message(FATAL_ERROR
        "W5500 OTBR requires CONFIG_LWIP_PPP_SUPPORT=n (lwIP/OTBR ABI mismatch)")
endif()

idf_component_register(
    SRCS         "thbr_main.c" ${THBR_TRANSPORT_SRC} ${THBR_EXTRA_SRC}
                 "log_sink.c" "info_server.c" "ble_proxy.c" "heap_probe.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES openthread esp_netif esp_event esp_eth
                  esp_driver_spi esp_driver_gpio esp_driver_usb_serial_jtag bt
                  espressif__mdns nvs_flash vfs lwip esp_hw_support spi_flash
                  spiffs espressif__esp_ot_br_server esp_http_server esp_timer
                  espressif__cjson espressif__esp_websocket_client mbedtls
    WHOLE_ARCHIVE
)

# Only the point-to-point USB transports need the synthetic-MAC wrapper.
if(NOT CONFIG_THBR_TRANSPORT_W5500)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_netif_get_mac")
endif()