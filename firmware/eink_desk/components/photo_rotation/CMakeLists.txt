idf_component_register(
    SRCS "photo_rotation.c"
    INCLUDE_DIRS "."
    REQUIRES esp_system photo_store
)
