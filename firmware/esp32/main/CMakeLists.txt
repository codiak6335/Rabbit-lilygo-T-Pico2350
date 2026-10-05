idf_component_register(
    SRCS
        "app_main.cpp"
        "audio_api.cpp"
        "deckscript_bridge.cpp"
        "lcd_status.cpp"
        "wifi_provision.cpp"
        "web_ui.cpp"
        "../../core/src/pool_geometry.cpp"
        "../../core/src/workout_engine.cpp"
        "../../protocol/src/framing.cpp"
        "../../protocol/src/plan_codec.cpp"
        "../../protocol/src/workout_service.cpp"
        "../../protocol/src/audio_service.cpp"
    INCLUDE_DIRS
        "."
        "../../core/include"
        "../../protocol/include"
    REQUIRES
        driver
        esp_driver_usb_serial_jtag
        esp_event
        esp_http_server
        esp_lcd
        esp_netif
        esp_timer
        esp_wifi
        json
        nvs_flash
)

set(RABBIT_WEB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
target_add_binary_data(${COMPONENT_LIB} "${RABBIT_WEB_ROOT}/index.html" TEXT RENAME_TO rabbit_index_html)
target_add_binary_data(${COMPONENT_LIB} "${RABBIT_WEB_ROOT}/css/styles.css" TEXT RENAME_TO rabbit_styles_css)
target_add_binary_data(${COMPONENT_LIB} "${RABBIT_WEB_ROOT}/js/deckscript.js" TEXT RENAME_TO rabbit_deckscript_js)
target_add_binary_data(${COMPONENT_LIB} "${RABBIT_WEB_ROOT}/js/navigation.js" TEXT RENAME_TO rabbit_navigation_js)
target_add_binary_data(${COMPONENT_LIB} "${RABBIT_WEB_ROOT}/db/pools.json" TEXT RENAME_TO rabbit_pools_json)
target_add_binary_data(${COMPONENT_LIB} "${RABBIT_WEB_ROOT}/db/sets.json" TEXT RENAME_TO rabbit_initial_sets_json)
target_add_binary_data(${COMPONENT_LIB} "${RABBIT_WEB_ROOT}/static/deckscript-chatgpt-prompt.txt" TEXT RENAME_TO rabbit_deckscript_prompt)
target_add_binary_data(${COMPONENT_LIB} "${RABBIT_WEB_ROOT}/favicon.ico" BINARY RENAME_TO rabbit_favicon_ico)

target_add_binary_data(${COMPONENT_LIB} "${RABBIT_WEB_ROOT}/js/audio.js" TEXT RENAME_TO rabbit_audio_js)
