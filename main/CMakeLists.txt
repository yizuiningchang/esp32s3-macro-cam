set(SOURCES
    "main.c"
    "app_config.c"
    "app_power.c"
    "app_camera.c"
    "app_storage.c"
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
)
