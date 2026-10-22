idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES battery_service display_service heap input_service network_service photo_rotation photo_store todo_store ui
)
