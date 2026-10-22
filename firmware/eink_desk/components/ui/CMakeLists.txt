idf_component_register(
    SRCS "diagnostics_screen.c" "home_screen.c" "photo_album_screen.c" "placeholder_screen.c" "todo_screen.c"
    INCLUDE_DIRS "."
    REQUIRES battery_service graphics photo_store
)
