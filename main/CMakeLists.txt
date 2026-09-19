set(SOURCES
    "main.c"
    "app_config.c"
    "app_session.c"
    "app_power.c"
    "app_camera.c"
    "app_storage.c"
    "app_audio.c"
    "app_led.c"
    "app_light.c"
    "app_cmd.c"
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
)
# ⚠ 不要给 main 加显式 REQUIRES：main 一旦声明 REQUIRES 就失去"自动依赖全部组件"
#   的行为，nvs.h / esp_app_desc.h 等立刻找不到（2026-09-19 实测踩坑）。
#   需要扩展芯片私有驱动时，在 .c 里写 extern 原型即可（该组件是 BSP 的直接依赖，必然链接）。
