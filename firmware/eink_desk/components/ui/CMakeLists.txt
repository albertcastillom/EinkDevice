idf_component_register(
    SRCS "diagnostics_screen.c" "todo_screen.c"
    INCLUDE_DIRS "."
    REQUIRES battery_service graphics
)
