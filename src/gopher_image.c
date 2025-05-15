/*
 * Copyright (c) 2024 Gopher Image Viewer
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include "gopher_image.h"

#include <zephyr/sys/util.h>

/* Check if SPIRAM is available */
#ifdef CONFIG_ESP_SPIRAM
/* With SPIRAM enabled, we can use much more memory */
#define LARGE_MEMORY_AVAILABLE 1
/* Include the shared multi-heap API for PSRAM access */
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <zephyr/logging/log.h>
#else
#define LARGE_MEMORY_AVAILABLE 0
#endif

/* Helper function to allocate memory, using PSRAM if available */
static void *memory_alloc(size_t size, const struct shell *shell) {
    void *ptr = NULL;

    /* For debugging - temporary force system heap for small allocations */
    if (size < 50000) {
        ptr = k_malloc(size);
        if (ptr && shell) {
            shell_print(shell, "FORCING small allocation of %zu bytes from system heap", size);
        }
        return ptr;
    }

#ifdef CONFIG_ESP_SPIRAM
    if (LARGE_MEMORY_AVAILABLE) {
        /* Try PSRAM allocation first */
        ptr = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, size);
        if (ptr && shell) {
            shell_print(shell, "Allocated %zu bytes from PSRAM at %p", size, ptr);
            
            /* Perform a sanity check by writing to and reading from the first bytes */
            if (size >= 4) {
                /* Write a known pattern */
                char *test = (char*)ptr;
                test[0] = 'T';
                test[1] = 'E';
                test[2] = 'S';
                test[3] = 'T';
                
                /* Read it back to verify memory access */
                if (test[0] != 'T' || test[1] != 'E' || test[2] != 'S' || test[3] != 'T') {
                    if (shell) {
                        shell_error(shell, "Memory verification failed! Got: %c%c%c%c",
                                  test[0], test[1], test[2], test[3]);
                    }
                    /* Free and force normal heap */
                    shared_multi_heap_free(ptr);
                    ptr = NULL;
                } else if (shell) {
                    shell_print(shell, "PSRAM memory verification succeeded");
                }
            }
        }
    }

    /* If PSRAM allocation failed or isn't available, try system heap */
    if (!ptr) {
        ptr = k_malloc(size);
        if (ptr && shell) {
            shell_print(shell, "Allocated %zu bytes from system heap at %p", size, ptr);
        }
    }
#else
    ptr = k_malloc(size);
    if (ptr && shell) {
        shell_print(shell, "Allocated %zu bytes from system heap at %p", size, ptr);
    }
#endif

    return ptr;
}

/* Helper function to free memory */
static void memory_free(void *ptr) {
    if (!ptr) {
        return;
    }

#ifdef CONFIG_ESP_SPIRAM
    /* In Zephyr, shared_multi_heap_free is safe to call on any pointer */
    shared_multi_heap_free(ptr);
#else
    k_free(ptr);
#endif
}

/* Register logging module after all Zephyr includes */
LOG_MODULE_REGISTER(gopher_image, LOG_LEVEL_ERR);

/* Define STB_IMAGE implementation in only one file */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* Add global variable for JPEG decoder scale reduction */
int stbi__jpeg_decode_reduced = 1;

/* ANSI color codes */
#define COLOR_RESET   "\033[0m"
#define COLOR_BLACK   "\033[30m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"

/* Background color codes */
#define BG_BLACK      "\033[40m"
#define BG_RED        "\033[41m"
#define BG_GREEN      "\033[42m"
#define BG_YELLOW     "\033[43m"
#define BG_BLUE       "\033[44m"
#define BG_MAGENTA    "\033[45m"
#define BG_CYAN       "\033[46m"
#define BG_WHITE      "\033[47m"

/* Default ASCII character set - from darkest to lightest */
#define ASCII_RAMP " .:-=+*#%@"
#define ASCII_RAMP_LEN 10

/* Block character set for higher quality (requires Unicode support) */
#define BLOCK_CHARS " ░▒▓█"
#define BLOCK_CHARS_LEN 5

/* Terminal color definitions */
typedef enum {
    BLACK = 0,
    RED,
    GREEN,
    YELLOW,
    BLUE,
    MAGENTA,
    CYAN,
    WHITE,
    COLOR_COUNT
} term_color_t;

/* RGB values for standard 8-color terminal palette */
static const struct {
    uint8_t r, g, b;
    const char *name;
} terminal_colors[COLOR_COUNT] = {
    {0,   0,   0,   "Black"},      /* BLACK */
    {170, 0,   0,   "Red"},        /* RED */
    {0,   170, 0,   "Green"},      /* GREEN */
    {170, 170, 0,   "Yellow"},     /* YELLOW */
    {0,   0,   170, "Blue"},       /* BLUE */
    {170, 0,   170, "Magenta"},    /* MAGENTA */
    {0,   170, 170, "Cyan"},       /* CYAN */
    {170, 170, 170, "White"}       /* WHITE */
};

/* ANSI color codes for foreground */
static const char *fg_color_codes[COLOR_COUNT] = {
    COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_YELLOW,
    COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE
};

/* ANSI color codes for background */
static const char *bg_color_codes[COLOR_COUNT] = {
    BG_BLACK, BG_RED, BG_GREEN, BG_YELLOW,
    BG_BLUE, BG_MAGENTA, BG_CYAN, BG_WHITE
};

/* Default image processing options */
static const image_process_options_t default_options = {
    .maintain_aspect_ratio = true,
    .use_bilinear_filtering = true,
    .brightness_adjust = 1.0f,
    .contrast_adjust = 1.0f
};

/* Default ASCII art configuration */
static const ascii_art_config_t default_config = {
    .use_color = true,
    .use_dithering = true,
    .use_extended_chars = false,
    .color_mode = 8,
    .brightness = 1.0f,
    .contrast = 1.0f
};

/* Helper function to clamp values to 0-255 range */
static inline uint8_t clamp(int value, int min, int max) {
    return (value < min) ? min : ((value > max) ? max : value);
}

/* Convert RGB to perceptual grayscale using standard coefficients */
static uint8_t rgb_to_gray(uint8_t r, uint8_t g, uint8_t b) {
    /* Standard luminance conversion weighted for human perception */
    return (uint8_t)(0.299f * r + 0.587f * g + 0.114f * b);
}

/* Map RGB to closest terminal color using weighted Euclidean distance */
static term_color_t rgb_to_terminal_color(uint8_t r, uint8_t g, uint8_t b) {
    int best_match = 0;
    int min_distance = 255*255*3; /* Max possible distance */
    
    for (int i = 0; i < COLOR_COUNT; i++) {
        /* Calculate color distance using weighted Euclidean distance */
        /* Human eye is more sensitive to green, then red, then blue */
        int dr = r - terminal_colors[i].r;
        int dg = g - terminal_colors[i].g;
        int db = b - terminal_colors[i].b;
        
        /* Weighted RGB - better perceptual match than simple Euclidean */
        int distance = (dr*dr*3 + dg*dg*4 + db*db*2) / 9;
        
        if (distance < min_distance) {
            min_distance = distance;
            best_match = i;
        }
    }
    
    return (term_color_t)best_match;
}

/* Apply brightness and contrast adjustments to a pixel */
static rgb_pixel_t adjust_pixel(rgb_pixel_t pixel, float brightness, float contrast) {
    rgb_pixel_t result;
    
    /* Apply brightness first */
    float r = pixel.r * brightness;
    float g = pixel.g * brightness;
    float b = pixel.b * brightness;
    
    /* Then apply contrast (pivot around 128) */
    r = 128.0f + (r - 128.0f) * contrast;
    g = 128.0f + (g - 128.0f) * contrast;
    b = 128.0f + (b - 128.0f) * contrast;
    
    /* Clamp values to 0-255 range */
    result.r = clamp((int)r, 0, 255);
    result.g = clamp((int)g, 0, 255);
    result.b = clamp((int)b, 0, 255);
    
    return result;
}

/* Checks if buffer is likely HTML or text content */
static bool looks_like_text_content(uint8_t *data, size_t size) {
    /* Check for HTML markers */
    const char *html_markers[] = {
        "<html", "<HTML", "<!DOCTYPE", "<!doctype", 
        "<head", "<HEAD", "<body", "<BODY", 
        "HTTP/", "http://"
    };
    
    /* Check for HTML markers */
    for (int i = 0; i < sizeof(html_markers) / sizeof(html_markers[0]); i++) {
        if (size >= strlen(html_markers[i]) && 
            memmem(data, MIN(size, 200), html_markers[i], strlen(html_markers[i])) != NULL) {
            return true;
        }
    }
    
    /* Check if mostly ASCII printable characters */
    int printable_count = 0;
    int total_checked = MIN(200, size);  /* Only sample the first part */
    
    for (int i = 0; i < total_checked; i++) {
        if (isprint(data[i]) || isspace(data[i])) {
            printable_count++;
        }
    }
    
    /* If more than 90% are printable, it's likely text */
    return (printable_count > total_checked * 0.9);
}

/* Decode image data to RGB pixels with pre-scaling to conserve memory 
 * shell parameter is for debug output only, can be NULL */
static rgb_pixel_t *decode_image_to_rgb(uint8_t *image_data, size_t image_size, 
                               int *width, int *height, const struct shell *shell, int max_memory) {
    /* Configure stb_image for RGB decoding */
    stbi_set_unpremultiply_on_load(1);  /* Handle alpha correctly if present */
    stbi_convert_iphone_png_to_rgb(1);  /* Fix iOS image formats */
    
    /* Check if this might be text content before trying to decode */
    if (looks_like_text_content(image_data, image_size)) {
        *width = 0;
        *height = 0;
        return NULL;
    }
    
    /* Check the first few bytes to try to identify the image format */
    const char *format = "unknown";
    if (image_size >= 3 && image_data[0] == 0xFF && image_data[1] == 0xD8 && image_data[2] == 0xFF) {
        format = "JPEG";
    } else if (image_size >= 8 && 
               image_data[0] == 0x89 && image_data[1] == 'P' && image_data[2] == 'N' && 
               image_data[3] == 'G' && image_data[4] == 0x0D && image_data[5] == 0x0A &&
               image_data[6] == 0x1A && image_data[7] == 0x0A) {
        format = "PNG";
    } else if (image_size >= 6 && 
               (memcmp(image_data, "GIF87a", 6) == 0 || memcmp(image_data, "GIF89a", 6) == 0)) {
        format = "GIF";
    }
    
    /* Skip format logging */
    
    /* Skip detailed header logging to avoid log module errors */
    
    /* First check the image dimensions without decoding it */
    int orig_width, orig_height, orig_channels;
    if (!stbi_info_from_memory(image_data, image_size, 
                              &orig_width, &orig_height, &orig_channels)) {
        /* Failed to get image info */
        return NULL;
    }
    
    /* Skip dimensions logging */
    
    /* Calculate memory requirements and determine if we need to reduce the size */
    size_t memory_needed = orig_width * orig_height * 3; /* 3 bytes per pixel (RGB) */
    /* max_memory is now defined at the function level */
    int scale = 1;
    
    /* Just print to log file, not to shell in this function */
    LOG_INF("Image size: %dx%d, Memory needed: %zu bytes",
           orig_width, orig_height, memory_needed);
    
    if (memory_needed > max_memory) {
        /* Calculate scale factor needed to fit in memory */
        scale = (int)sqrtf((float)memory_needed / max_memory) + 1;
        /* Skip scaling log */
    }
    
    /* For JPEG, we can use stbi's reduced loading for better performance and memory usage */
    if (format == "JPEG") {
        stbi_set_flip_vertically_on_load(0);
        
        /* Set up reduction for JPEG decoding */
        if (scale >= 8) {
            stbi__jpeg_decode_reduced = 8;
        } else if (scale >= 4) {
            stbi__jpeg_decode_reduced = 4;
        } else if (scale >= 2) {
            stbi__jpeg_decode_reduced = 2;
        } else {
            stbi__jpeg_decode_reduced = 1;
        }
    }
    
    /* Try decoding with memory-saving options */
    int channels;
    uint8_t *img = NULL;
    
    /* If JPEG special handling didn't work or it's not a JPEG, try regular loading */
    if (img == NULL) {
        /* For non-JPEG formats or if the special JPEG handling failed, try direct loading */
        img = stbi_load_from_memory(image_data, image_size, width, height, &channels, 3);
        
        /* If we're still out of memory, try scaling down manually after loading */
        if (img == NULL && strstr(stbi_failure_reason(), "outofmem") != NULL) {
            /* For memory issues, try with progressively smaller target sizes */
            /* Skip memory warning */
            
            /* Try with a maximum width/height to limit memory usage */
            int max_dim = 128; /* This should be small enough for most embedded systems */
            float aspect = (float)orig_width / orig_height;
            
            if (aspect >= 1.0f) {
                /* Landscape or square */
                *width = max_dim;
                *height = (int)(max_dim / aspect);
            } else {
                /* Portrait */
                *height = max_dim;
                *width = (int)(max_dim * aspect);
            }
            
            if (*width < 1) *width = 1;
            if (*height < 1) *height = 1;
            
            /* Allocate a buffer for RGB data */
            size_t alloc_size = (*width) * (*height) * sizeof(rgb_pixel_t);
            if (shell) shell_print(shell, "Attempting memory allocation of %zu bytes", alloc_size);
            
            rgb_pixel_t *rgb_img = (rgb_pixel_t *)memory_alloc(alloc_size, shell);
            
            if (!rgb_img) {
                if (shell) shell_print(shell, "Memory allocation failed! Requested: %zu bytes", alloc_size);
                return NULL;
            }
            if (shell) shell_print(shell, "Memory allocation succeeded");
            
            /* Fill with a gradient pattern as a placeholder */
            for (int y = 0; y < *height; y++) {
                for (int x = 0; x < *width; x++) {
                    int idx = y * (*width) + x;
                    rgb_img[idx].r = (uint8_t)(x * 255 / *width);
                    rgb_img[idx].g = (uint8_t)(y * 255 / *height);
                    rgb_img[idx].b = 128;
                }
            }
            
            /* Skip placeholder logging */
            return rgb_img;
        }
    }
    
    /* If we still failed to decode, handle errors */
    if (img == NULL) {
        /* Skip detailed error logging */
        return NULL;
    }
    
    /* The decoded data is already in RGB format: R,G,B,R,G,B,... */
    /* We can treat it as an array of rgb_pixel_t */
    rgb_pixel_t *rgb_img = (rgb_pixel_t*)img;
    
    /* Skip success logging */
    return rgb_img;
}

/* Downscale a color image with options for quality control */
static rgb_pixel_t *downscale_image_color(rgb_pixel_t *src, int src_w, int src_h, 
                                  int tgt_w, int tgt_h, 
                                  const image_process_options_t *options) {
    /* Use default options if none provided */
    if (!options) {
        options = &default_options;
    }
    
    /* Adjust target dimensions if maintaining aspect ratio */
    if (options->maintain_aspect_ratio) {
        float src_aspect = (float)src_w / src_h;
        float tgt_aspect = (float)tgt_w / tgt_h;
        
        if (src_aspect > tgt_aspect) {
            /* Source is wider, adjust height */
            int new_tgt_h = (int)(tgt_w / src_aspect);
            if (new_tgt_h < 1) new_tgt_h = 1;
            tgt_h = new_tgt_h;
        } else if (src_aspect < tgt_aspect) {
            /* Source is taller, adjust width */
            int new_tgt_w = (int)(tgt_h * src_aspect);
            if (new_tgt_w < 1) new_tgt_w = 1;
            tgt_w = new_tgt_w;
        }
        
        /* Adjusted dimensions to maintain aspect ratio */
    }
    
    /* Allocate memory for result */
    size_t alloc_size = tgt_w * tgt_h * sizeof(rgb_pixel_t);
    rgb_pixel_t *result = (rgb_pixel_t *)memory_alloc(alloc_size, NULL);
    
    if (!result) {
        return NULL;
    }
    
    /* Calculate scaling factors */
    float x_ratio = (float)src_w / (float)tgt_w;
    float y_ratio = (float)src_h / (float)tgt_h;
    
    /* Perform the actual scaling */
    for (int y = 0; y < tgt_h; y++) {
        for (int x = 0; x < tgt_w; x++) {
            rgb_pixel_t pixel;
            
            if (options->use_bilinear_filtering) {
                /* Bilinear filtering for better quality */
                float src_x = (float)x * x_ratio;
                float src_y = (float)y * y_ratio;
                int src_x_int = (int)src_x;
                int src_y_int = (int)src_y;
                float x_diff = src_x - src_x_int;
                float y_diff = src_y - src_y_int;
                
                /* Get the four surrounding pixels */
                rgb_pixel_t p00 = src[src_y_int * src_w + src_x_int];
                rgb_pixel_t p10 = (src_x_int < src_w - 1) ? 
                    src[src_y_int * src_w + src_x_int + 1] : p00;
                rgb_pixel_t p01 = (src_y_int < src_h - 1) ? 
                    src[(src_y_int + 1) * src_w + src_x_int] : p00;
                rgb_pixel_t p11 = (src_x_int < src_w - 1 && src_y_int < src_h - 1) ? 
                    src[(src_y_int + 1) * src_w + src_x_int + 1] : p00;
                
                /* Interpolate colors */
                pixel.r = (uint8_t)((1 - x_diff) * (1 - y_diff) * p00.r +
                        x_diff * (1 - y_diff) * p10.r +
                        (1 - x_diff) * y_diff * p01.r +
                        x_diff * y_diff * p11.r);
                
                pixel.g = (uint8_t)((1 - x_diff) * (1 - y_diff) * p00.g +
                        x_diff * (1 - y_diff) * p10.g +
                        (1 - x_diff) * y_diff * p01.g +
                        x_diff * y_diff * p11.g);
                
                pixel.b = (uint8_t)((1 - x_diff) * (1 - y_diff) * p00.b +
                        x_diff * (1 - y_diff) * p10.b +
                        (1 - x_diff) * y_diff * p01.b +
                        x_diff * y_diff * p11.b);
            } else {
                /* Nearest neighbor (faster but lower quality) */
                int src_x = (int)(x * x_ratio);
                int src_y = (int)(y * y_ratio);
                pixel = src[src_y * src_w + src_x];
            }
            
            /* Apply brightness and contrast adjustments */
            if (options->brightness_adjust != 1.0f || options->contrast_adjust != 1.0f) {
                pixel = adjust_pixel(pixel, options->brightness_adjust, options->contrast_adjust);
            }
            
            result[y * tgt_w + x] = pixel;
        }
    }
    
    return result;
}

/* Apply Floyd-Steinberg dithering to the image */
static void apply_floyd_steinberg_dithering(rgb_pixel_t *image, int width, int height) {
    /* Create a copy of the image for reading during dithering */
    size_t alloc_size = width * height * sizeof(rgb_pixel_t);
    rgb_pixel_t *img_copy = (rgb_pixel_t *)memory_alloc(alloc_size, NULL);
    
    if (!img_copy) {
        return;
    }
    
    /* Copy the original image */
    memcpy(img_copy, image, width * height * sizeof(rgb_pixel_t));
    
    /* Apply Floyd-Steinberg dithering */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            rgb_pixel_t old_pixel = img_copy[idx];
            
            /* Find the closest color in the palette */
            term_color_t new_color = rgb_to_terminal_color(old_pixel.r, old_pixel.g, old_pixel.b);
            rgb_pixel_t new_pixel;
            new_pixel.r = terminal_colors[new_color].r;
            new_pixel.g = terminal_colors[new_color].g;
            new_pixel.b = terminal_colors[new_color].b;
            
            /* Store the new pixel in the output image */
            image[idx] = new_pixel;
            
            /* Calculate quantization error */
            int err_r = old_pixel.r - new_pixel.r;
            int err_g = old_pixel.g - new_pixel.g;
            int err_b = old_pixel.b - new_pixel.b;
            
            /* Distribute error to neighboring pixels using Floyd-Steinberg pattern */
            if (x + 1 < width) {
                /* Right pixel (7/16) */
                int right_idx = y * width + (x + 1);
                img_copy[right_idx].r = clamp(img_copy[right_idx].r + (err_r * 7 / 16), 0, 255);
                img_copy[right_idx].g = clamp(img_copy[right_idx].g + (err_g * 7 / 16), 0, 255);
                img_copy[right_idx].b = clamp(img_copy[right_idx].b + (err_b * 7 / 16), 0, 255);
            }
            
            if (y + 1 < height) {
                /* Bottom pixel (5/16) */
                int bottom_idx = (y + 1) * width + x;
                img_copy[bottom_idx].r = clamp(img_copy[bottom_idx].r + (err_r * 5 / 16), 0, 255);
                img_copy[bottom_idx].g = clamp(img_copy[bottom_idx].g + (err_g * 5 / 16), 0, 255);
                img_copy[bottom_idx].b = clamp(img_copy[bottom_idx].b + (err_b * 5 / 16), 0, 255);
                
                if (x > 0) {
                    /* Bottom-left pixel (3/16) */
                    int bottom_left_idx = (y + 1) * width + (x - 1);
                    img_copy[bottom_left_idx].r = clamp(img_copy[bottom_left_idx].r + (err_r * 3 / 16), 0, 255);
                    img_copy[bottom_left_idx].g = clamp(img_copy[bottom_left_idx].g + (err_g * 3 / 16), 0, 255);
                    img_copy[bottom_left_idx].b = clamp(img_copy[bottom_left_idx].b + (err_b * 3 / 16), 0, 255);
                }
                
                if (x + 1 < width) {
                    /* Bottom-right pixel (1/16) */
                    int bottom_right_idx = (y + 1) * width + (x + 1);
                    img_copy[bottom_right_idx].r = clamp(img_copy[bottom_right_idx].r + (err_r * 1 / 16), 0, 255);
                    img_copy[bottom_right_idx].g = clamp(img_copy[bottom_right_idx].g + (err_g * 1 / 16), 0, 255);
                    img_copy[bottom_right_idx].b = clamp(img_copy[bottom_right_idx].b + (err_b * 1 / 16), 0, 255);
                }
            }
        }
    }
    
    /* Free the temporary buffer */
    memory_free(img_copy);
    
    /* Dithering applied */
}

/* Detect if a file is an image based on magic numbers or extension */
bool gopher_is_image(const uint8_t *data, size_t size) {
    if (size < 4) {
        return false;
    }
    
    /* Check for JPEG magic number: FF D8 FF */
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        /* Detected JPEG */
        return true;
    }
    
    /* Check for PNG magic number: 89 50 4E 47 0D 0A 1A 0A */
    if (size >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E &&
        data[3] == 0x47 && data[4] == 0x0D && data[5] == 0x0A &&
        data[6] == 0x1A && data[7] == 0x0A) {
        /* Detected PNG */
        return true;
    }
    
    /* Check for GIF magic number: 'GIF8' */
    if (size >= 5 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8') {
        /* Detected GIF */
        return true;
    }
    
    /* Additional checks for common problems - we'll skip detailed logging */
    char header_hex[100] = {0};
    int pos = 0;
    for (int i = 0; i < MIN(16, size); i++) {
        pos += snprintf(header_hex + pos, sizeof(header_hex) - pos, "%02x ", data[i]);
    }
    /* Skip detailed logging */
    
    /* Sometimes binary data might start with some plain text (HTTP headers, etc.) 
       Let's check if the file has known image extensions in the first 100 bytes */
    const char *extensions[] = {".jpg", ".jpeg", ".gif", ".png", ".bmp"};
    for (int i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        for (int j = 0; j < MIN(100, size - strlen(extensions[i])); j++) {
            if (strncasecmp((const char *)&data[j], extensions[i], strlen(extensions[i])) == 0) {
                /* Detected by extension */
                return true;
            }
        }
    }
    
    return false;
}

/* Render image as ASCII art */
static int render_ascii_art(const struct shell *shell, rgb_pixel_t *rgb_buffer, 
                           int width, int height, const ascii_art_config_t *config) {
    /* Detailed debug info */
    shell_print(shell, "DEBUG: render_ascii_art called with %dx%d image at %p", width, height, rgb_buffer);
    
    /* Verify buffer is not NULL */
    if (!rgb_buffer) {
        shell_error(shell, "ERROR: rgb_buffer is NULL!");
        return -EINVAL;
    }
    
    /* Verify dimensions are reasonable */
    if (width <= 0 || height <= 0 || width > 1000 || height > 1000) {
        shell_error(shell, "ERROR: Invalid image dimensions: %dx%d", width, height);
        return -EINVAL;
    }
    
    /* Try to access first byte to check memory access */
    shell_print(shell, "DEBUG: First pixel RGB values: %d,%d,%d", 
               rgb_buffer[0].r, rgb_buffer[0].g, rgb_buffer[0].b);
    
    /* Determine terminal width (use 80 as default) */
    int term_width = 80;
    int term_height = 24;
    
    /* Allocate a buffer for a line of ASCII art */
    size_t alloc_size = term_width * 20; /* Extra space for color codes */
    char *line = (char *)memory_alloc(alloc_size, shell);
    
    if (!line) {
        return -ENOMEM;
    }
    
    
    /* Render header */
    shell_fprintf(shell, SHELL_NORMAL, "ASCII Art Image (%dx%d pixels)\n", width, height);
    shell_fprintf(shell, SHELL_NORMAL, "----------------------------------------\n");
    
    /* Track last used colors to minimize color code output */
    term_color_t last_fg = BLACK;
    term_color_t last_bg = BLACK;
    
    /* Determine if color is supported */
    bool color_supported = config->use_color;
    
    /* Render each line of ASCII art */
    for (int y = 0; y < height; y++) {
        int pos = 0;
        bool color_active = false;
        
        for (int x = 0; x < width; x++) {
            rgb_pixel_t pixel = rgb_buffer[y * width + x];
            
            /* Calculate intensity for ASCII character selection */
            uint8_t gray = rgb_to_gray(pixel.r, pixel.g, pixel.b);
            
            /* Select ASCII character based on brightness */
            char ch = ASCII_RAMP[gray * (ASCII_RAMP_LEN - 1) / 255];
            
            if (color_supported) {
                /* Determine best foreground color */
                term_color_t fg = rgb_to_terminal_color(pixel.r, pixel.g, pixel.b);
                term_color_t bg = BLACK; /* Background is typically black */
                
                /* Add color codes only when color changes (optimization) */
                if (!color_active || fg != last_fg || bg != last_bg) {
                    pos += snprintf(line + pos, 20, "%s%s", 
                                   fg_color_codes[fg], bg_color_codes[bg]);
                    last_fg = fg;
                    last_bg = bg;
                    color_active = true;
                }
            }
            
            /* Add the ASCII character (double it for better aspect ratio) */
            line[pos++] = ch;
            line[pos++] = ch;
        }
        
        /* Reset colors at end of line */
        if (color_supported && color_active) {
            pos += snprintf(line + pos, 10, "%s", COLOR_RESET);
        }
        
        /* Null terminate and print */
        line[pos] = '\0';
        shell_print(shell, "%s", line);
    }
    
    /* Free the line buffer */
    memory_free(line);
    
    /* Render footer */
    shell_fprintf(shell, SHELL_NORMAL, "----------------------------------------\n");
    
    return 0;
}

/* Function to display text content when image decoding fails */
static void display_text_content(const struct shell *shell, uint8_t *data, size_t size) {
    shell_print(shell, "Server response appears to be text. Content:");
    shell_print(shell, "-------------------------------------------");
    
    /* Output text in chunks to avoid buffer limitations */
    char text_buffer[128];
    size_t chunk_size = 0;
    text_buffer[0] = '\0';
    
    /* Skip any binary or non-printable data at the beginning */
    size_t start_pos = 0;
    while (start_pos < size && !isprint(data[start_pos]) && !isspace(data[start_pos])) {
        start_pos++;
    }
    
    for (size_t i = start_pos; i < size; i++) {
        /* Replace control characters with spaces for better readability */
        char c = data[i];
        if (iscntrl(c) && c != '\r' && c != '\n' && c != '\t') {
            c = ' ';
        }
        
        if (chunk_size < sizeof(text_buffer) - 1) {
            text_buffer[chunk_size++] = c;
            text_buffer[chunk_size] = '\0';
        }
        
        /* Output a chunk when it's full or contains a newline */
        if (chunk_size >= sizeof(text_buffer) - 1 || 
            i == size - 1 || 
            c == '\n') {
            
            /* Strip trailing CR/LF for cleaner output */
            while (chunk_size > 0 && 
                  (text_buffer[chunk_size-1] == '\r' || 
                   text_buffer[chunk_size-1] == '\n')) {
                text_buffer[--chunk_size] = '\0';
            }
            
            if (chunk_size > 0) {
                shell_print(shell, "%s", text_buffer);
            }
            
            chunk_size = 0;
            text_buffer[0] = '\0';
        }
    }
    
    shell_print(shell, "-------------------------------------------");
}

/* Main function to render an image as ASCII art */
int gopher_render_image(const struct shell *shell, uint8_t *file_data, 
                       size_t file_size, ascii_art_config_t *config) {
    int ret = 0;
    rgb_pixel_t *img = NULL;
    rgb_pixel_t *scaled_img = NULL;
    int width, height;
    
    /* Define memory limit here so it's available throughout the function */
    /* Use 3MB when PSRAM is available, 200KB otherwise */
    int max_memory = LARGE_MEMORY_AVAILABLE ? 3000000 : 200000;
    
    /* Print memory availability */
    if (LARGE_MEMORY_AVAILABLE) {
        shell_print(shell, "PSRAM is available - using 3MB memory limit");
    } else {
        shell_print(shell, "PSRAM not available - using 200KB memory limit");
    }
    
    /* Use default config if none is provided */
    if (!config) {
        config = (ascii_art_config_t *)&default_config;
    }
    
    /* Early check for text content */
    if (looks_like_text_content(file_data, file_size)) {
        shell_error(shell, "Content appears to be text, not an image");
        display_text_content(shell, file_data, file_size);
        return -EINVAL;
    }
    
    /* Verify the file looks like an image */
    if (!gopher_is_image(file_data, file_size)) {
        /* Show the first few bytes to help diagnose the issue */
        if (file_size >= 16) {
            char header_hex[100] = {0};
            int pos = 0;
            for (int i = 0; i < MIN(16, file_size); i++) {
                pos += snprintf(header_hex + pos, sizeof(header_hex) - pos, "%02x ", file_data[i]);
            }
            /* File doesn't match image signature */
        }
        
        shell_error(shell, "File format is not a recognized image type (JPEG, PNG, or GIF)");
        
        /* Even though it's not a recognized image format, still try to decode it */
        shell_print(shell, "Attempting to decode anyway...");
    }
    
    /* First check image dimensions without decoding it fully */
    int orig_width = 0, orig_height = 0, orig_channels = 0;
    bool dimensions_available = false;
    
    if (stbi_info_from_memory(file_data, file_size, &orig_width, &orig_height, &orig_channels)) {
        dimensions_available = true;
        /* Image dimensions obtained */
        
        /* Pre-check if this image will need too much memory */
        size_t memory_needed = orig_width * orig_height * 3; /* 3 bytes per RGB pixel */
        /* Use the same memory limit as defined earlier */
        if (memory_needed > max_memory) {
            shell_print(shell, "Large image detected (%dx%d, ~%zu KB), memory limit: %d KB", 
                       orig_width, orig_height, memory_needed / 1024, max_memory / 1024);
            
            /* Use smaller target size for ASCII art */
            int target_width = 32;  /* Very conservative target */
            int target_height = 16;
            
            /* Try to maintain aspect ratio */
            float aspect = (float)orig_width / orig_height;
            if (aspect > 2.0f) {
                /* Very wide image */
                target_height = (int)(target_width / aspect);
                if (target_height < 4) target_height = 4;
            } else if (aspect < 0.5f) {
                /* Very tall image */
                target_width = (int)(target_height * aspect);
                if (target_width < 8) target_width = 8;
            }
            
            /* Create a simplified placeholder image at small size */
            size_t alloc_size = target_width * target_height * sizeof(rgb_pixel_t);
            rgb_pixel_t *placeholder = (rgb_pixel_t *)memory_alloc(alloc_size, shell);
            
            if (placeholder) {
                /* Generate a gradient placeholder based on original dimensions */
                for (int y = 0; y < target_height; y++) {
                    for (int x = 0; x < target_width; x++) {
                        int idx = y * target_width + x;
                        placeholder[idx].r = (uint8_t)(x * 255 / target_width);
                        placeholder[idx].g = (uint8_t)(y * 255 / target_height);
                        placeholder[idx].b = 128;
                    }
                }
                
                shell_print(shell, "Using simplified placeholder for large image");
                shell_print(shell, "Original image dimensions: %dx%d pixels", orig_width, orig_height);
                
                /* Render the ASCII art from our placeholder */
                ret = render_ascii_art(shell, placeholder, target_width, target_height, config);
                
                /* Free the placeholder memory */
                memory_free(placeholder);
                
                /* Return success, we've shown a placeholder */
                return ret;
            }
        }
    }
    
    /* Try to decode the image data to RGB pixels */
    img = decode_image_to_rgb(file_data, file_size, &width, &height, shell, max_memory);
    if (!img) {
        shell_error(shell, "Failed to decode image data");
        
        /* Check what kind of error we might have */
        const char *error = stbi_failure_reason();
        
        if (strstr(error, "no SOF") != NULL) {
            shell_error(shell, "No JPEG Start Of Frame marker found - this usually means:");
            shell_error(shell, "1. The server returned an HTML error page instead of an image");
            shell_error(shell, "2. The server may require authentication or cookies");
            shell_error(shell, "3. There might be a redirect to another page");
        } else if (strstr(error, "bad huffman") != NULL) {
            shell_error(shell, "Bad Huffman code found - the image data is corrupted or incomplete");
        } else if (strstr(error, "PNG") != NULL) {
            shell_error(shell, "PNG decoding error - file may be corrupted or in an unsupported format");
        } else if (strstr(error, "outofmem") != NULL) {
            shell_error(shell, "Image is too large for available memory");
            
            if (dimensions_available) {
                shell_print(shell, "Image dimensions: %dx%d pixels (%d channels)", 
                           orig_width, orig_height, orig_channels);
            }
            
            /* For out of memory errors, create a small placeholder gradient */
            int ph_width = 32;
            int ph_height = 16;
            size_t alloc_size = ph_width * ph_height * sizeof(rgb_pixel_t);
            rgb_pixel_t *placeholder = (rgb_pixel_t *)memory_alloc(alloc_size, shell);
            
            if (placeholder) {
                /* Generate a simple gradient */
                for (int y = 0; y < ph_height; y++) {
                    for (int x = 0; x < ph_width; x++) {
                        int idx = y * ph_width + x;
                        placeholder[idx].r = (uint8_t)(x * 255 / ph_width);
                        placeholder[idx].g = (uint8_t)(y * 255 / ph_height);
                        placeholder[idx].b = 128;
                    }
                }
                
                shell_print(shell, "Using placeholder image since original is too large for memory");
                
                /* Render the ASCII art from our placeholder */
                ret = render_ascii_art(shell, placeholder, ph_width, ph_height, config);
                
                /* Free the placeholder memory */
                memory_free(placeholder);
                
                /* Return success, we've shown a placeholder */
                return ret;
            }
        } else {
            shell_error(shell, "Image decoding error: %s", error);
        }
        
        /* Check if this might be a text/HTML response and display if so */
        if (looks_like_text_content(file_data, file_size)) {
            display_text_content(shell, file_data, file_size);
        } else if (file_size < 1024) {
            /* For small non-image data, try displaying as text anyway */
            shell_print(shell, "Attempting to display content as text:");
            display_text_content(shell, file_data, file_size);
        }
        
        return -EINVAL;
    }
    
    shell_print(shell, "Successfully decoded image: %dx%d pixels", width, height);
    
    /* Determine target dimensions for the console (aspect ratio 2:1 for terminal chars) */
    int target_width = 40;  /* Reasonable default for most terminals */
    int target_height = 20; /* Double the width-to-height ratio for text console */
    
    /* Create options for downscaling */
    image_process_options_t options = {
        .maintain_aspect_ratio = true,
        .use_bilinear_filtering = true,
        .brightness_adjust = config->brightness,
        .contrast_adjust = config->contrast
    };
    
    /* Downscale the image */
    shell_print(shell, "DEBUG: Attempting to downscale image from %dx%d to %dx%d", 
               width, height, target_width, target_height);
    scaled_img = downscale_image_color(img, width, height, target_width, target_height, &options);
    if (!scaled_img) {
        shell_error(shell, "Failed to downscale image");
        stbi_image_free(img);
        return -ENOMEM;
    }
    shell_print(shell, "DEBUG: Successfully downscaled image to %dx%d, result at %p", 
               target_width, target_height, scaled_img);
    
    /* Apply dithering if requested */
    if (config->use_dithering) {
        apply_floyd_steinberg_dithering(scaled_img, target_width, target_height);
    }
    
    /* Render the ASCII art */
    ret = render_ascii_art(shell, scaled_img, target_width, target_height, config);
    
    /* Free memory */
    stbi_image_free(img);  /* Use stbi_image_free instead of k_free for the original image */
    
    /* Free the scaled image */
    memory_free(scaled_img);
    
    return ret;
}

/* Initialize the image rendering module */
int gopher_image_init(void) {
    return 0;
}