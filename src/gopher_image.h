/*
 * Copyright (c) 2024 Gopher Image Viewer
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GOPHER_IMAGE_H_
#define GOPHER_IMAGE_H_

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

/* Structure for RGB pixel handling */
typedef struct {
    uint8_t r, g, b;
} rgb_pixel_t;

/* Options for image processing */
typedef struct {
    bool maintain_aspect_ratio;
    bool use_bilinear_filtering;
    float brightness_adjust;  /* 0.5-2.0, 1.0 is neutral */
    float contrast_adjust;    /* 0.5-2.0, 1.0 is neutral */
} image_process_options_t;

/* Configuration struct for ASCII art rendering */
typedef struct {
    bool use_color;          /* Use color or grayscale */
    bool use_dithering;      /* Apply dithering to improve color */
    bool use_extended_chars; /* Use extended ASCII for better resolution */
    int color_mode;          /* 8 or 16 colors */
    float brightness;        /* Brightness adjustment (0.5-2.0) */
    float contrast;          /* Contrast adjustment (0.5-2.0) */
} ascii_art_config_t;

/**
 * @brief Render an image file as ASCII art on the console
 *
 * @param shell Pointer to the shell instance
 * @param file_data Pointer to the file data buffer
 * @param file_size Size of the file data buffer
 * @param config Pointer to the ASCII art configuration (or NULL for default)
 * @return 0 on success, negative errno otherwise
 */
int gopher_render_image(const struct shell *shell, uint8_t *file_data, 
                        size_t file_size, ascii_art_config_t *config);

/**
 * @brief Determine if a file might be an image based on magic numbers
 *
 * @param data Pointer to the file data buffer
 * @param size Size of the file data buffer
 * @return true if the file is likely an image, false otherwise
 */
bool gopher_is_image(const uint8_t *data, size_t size);

/**
 * @brief Initialize the image rendering module
 *
 * @return 0 on success, negative errno otherwise
 */
int gopher_image_init(void);

#endif /* GOPHER_IMAGE_H_ */