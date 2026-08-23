if(CONFIG_HUB75_BACKEND_ARDUINO_GFX)
    set(app_srcs "main.cpp")
else()
    set(app_srcs "main_native.cpp" "font5x7.cpp")
endif()

idf_component_register(SRCS ${app_srcs} INCLUDE_DIRS ".")
