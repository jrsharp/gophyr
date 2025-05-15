/*
 * Copyright (c) 2023 Gopher Client
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GOPHER_CLIENT_H_
#define GOPHER_CLIENT_H_

#include <zephyr/kernel.h>

/* Maximum number of items in a directory listing */
#define GOPHER_MAX_DIR_ITEMS 64

/* Maximum length of selector strings */
#define GOPHER_MAX_SELECTOR_LEN 256

/* Maximum length of a server hostname */
#define GOPHER_MAX_HOSTNAME_LEN 64

/* Default Gopher port */
#define GOPHER_DEFAULT_PORT 70

/* Maximum buffer size for responses (increased for better image handling) */
#define GOPHER_BUFFER_SIZE 16384

/* Item type definitions per RFC 1436 */
#define GOPHER_TYPE_TEXT '0'
#define GOPHER_TYPE_DIRECTORY '1'
#define GOPHER_TYPE_CSO '2'
#define GOPHER_TYPE_ERROR '3'
#define GOPHER_TYPE_BINHEX '4'
#define GOPHER_TYPE_DOS '5'
#define GOPHER_TYPE_UUENCODED '6'
#define GOPHER_TYPE_SEARCH '7'
#define GOPHER_TYPE_TELNET '8'
#define GOPHER_TYPE_BINARY '9'
#define GOPHER_TYPE_REDUNDANT '+'
#define GOPHER_TYPE_TN3270 'T'
#define GOPHER_TYPE_GIF 'g'
#define GOPHER_TYPE_IMAGE 'I'

/* Structure to represent a Gopher item */
struct gopher_item {
    char type;
    char display_string[GOPHER_MAX_SELECTOR_LEN];
    char selector[GOPHER_MAX_SELECTOR_LEN];
    char hostname[GOPHER_MAX_HOSTNAME_LEN];
    uint16_t port;
};

/* Structure to represent the Gopher client state */
struct gopher_client {
    /* Current server information */
    char hostname[GOPHER_MAX_HOSTNAME_LEN];
    uint16_t port;
    
    /* Connection status */
    bool connected;
    
    /* Last directory listing */
    struct gopher_item items[GOPHER_MAX_DIR_ITEMS];
    int item_count;
    
    /* Navigation history */
    char history[10][GOPHER_MAX_SELECTOR_LEN];
    int history_pos;
    int history_count;
};

/**
 * @brief Initialize the Gopher client
 *
 * @param client Pointer to the client structure
 * @return 0 on success, negative errno otherwise
 */
int gopher_client_init(struct gopher_client *client);

/**
 * @brief Connect to a Gopher server
 *
 * @param client Pointer to the client structure
 * @param hostname Server hostname
 * @param port Server port (use 0 for default)
 * @return 0 on success, negative errno otherwise
 */
int gopher_connect(struct gopher_client *client, const char *hostname, uint16_t port);

/**
 * @brief Send a selector to the server and receive the response
 *
 * @param client Pointer to the client structure
 * @param selector Selector string to send
 * @param buffer Buffer to store the response
 * @param buffer_size Size of the buffer
 * @return Size of received data on success, negative errno otherwise
 */
int gopher_send_selector(struct gopher_client *client, const char *selector, 
                         char *buffer, size_t buffer_size);

/**
 * @brief [DEPRECATED] Update navigation history with a new selector
 * This function is now a stub - history management is handled directly in gopher_send_selector
 * 
 * @param client Pointer to the client structure
 * @param selector Selector string to add to history
 * @return 0 on success, negative errno otherwise
 */
int gopher_update_history(struct gopher_client *client, const char *selector);

/**
 * @brief Parse a directory listing from a buffer
 *
 * @param client Pointer to the client structure
 * @param buffer Buffer containing the directory listing
 * @return Number of items parsed on success, negative errno otherwise
 */
int gopher_parse_directory(struct gopher_client *client, const char *buffer);

/**
 * @brief Get string representation of item type
 * 
 * @param type Item type character
 * @return Pointer to string description
 */
const char *gopher_type_to_str(char type);

#endif /* GOPHER_CLIENT_H_ */
