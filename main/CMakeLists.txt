if(CONFIG_HUB75_BACKEND_ARDUINO_GFX)
    set(app_srcs "main.cpp")
else()
    set(app_srcs "main_native.cpp" "font5x7.cpp")
endif()

# HTTPS REST API (hd-wf2 branch only - see Kconfig.projbuild "HD-WF1 HTTPS API"): the cert/key are generated
# per board by tools/gen_https_cert.sh (gitignored, like sdkconfig) rather than committed, so they won't exist
# until that's been run - fail with a clear message instead of a confusing embed-time file-not-found error.
set(https_embed_files "")
if(CONFIG_HTTPS_API_ENABLE)
    set(cert_file "${CMAKE_CURRENT_LIST_DIR}/../certs/server_cert.pem")
    set(key_file "${CMAKE_CURRENT_LIST_DIR}/../certs/server_key.pem")
    if(NOT EXISTS "${cert_file}" OR NOT EXISTS "${key_file}")
        message(FATAL_ERROR "HTTPS_API_ENABLE is on but certs/server_cert.pem / certs/server_key.pem are missing - "
                             "run tools/gen_https_cert.sh first (see main/Kconfig.projbuild's HTTPS API menu).")
    endif()
    set(https_embed_files "${cert_file}" "${key_file}")
endif()

idf_component_register(SRCS ${app_srcs} INCLUDE_DIRS "."
                        EMBED_TXTFILES ${https_embed_files}
                        REQUIRES esp_app_format esp_http_server esp_https_server mbedtls esp_wifi esp_netif esp_event nvs_flash mqtt cjson esp_driver_gpio lwip esp_partition app_update)
