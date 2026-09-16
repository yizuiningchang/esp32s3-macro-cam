set(SOURCES
    "main.c"
    "app_config.c"
    "app_session.c"
    "app_power.c"
    "app_camera.c"
    "app_storage.c"
    "app_audio.c"
    "app_led.c"
    "app_cmd.c"
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
)
