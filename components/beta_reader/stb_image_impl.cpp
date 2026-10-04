#include "esp_heap_caps.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_REALLOC(p, newsz) heap_caps_realloc((p), (newsz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_FREE(p) heap_caps_free((p))
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
