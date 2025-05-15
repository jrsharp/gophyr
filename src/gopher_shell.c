/*
 * Copyright (c) 2023 Gopher Shell Commands
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "gopher_client.h"
#include "gopher_image.h"

/* Forward declarations of helper functions */
static int ensure_client_initialized(const struct shell *shell);
static int gopher_count_info_items(struct gopher_client *client);

/* ANSI color codes */
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"

#define GOPHER_STACK_SIZE 8192  /* Increased stack size for ESP32-S3 */
#define GOPHER_PRIORITY 7

static struct gopher_client client;
static char gopher_buffer[GOPHER_BUFFER_SIZE];
static bool net_initialized = false;
static bool client_initialized = false;

/* Helper function to count info items in directory listing */
static int gopher_count_info_items(struct gopher_client *client)
{
    int info_count = 0;
    for (int i = 0; i < client->item_count; i++) {
        if (client->items[i].type == 'i') {
            info_count++;
        }
    }
    return info_count;
}

/* Helper function to ensure client is initialized */
static int ensure_client_initialized(const struct shell *shell)
{
    int ret;
    
    if (client_initialized) {
        /* Even if already initialized, check if client is in a valid state */
        if (client.connected && client.hostname[0] == '\0') {
            /* Inconsistent state detected - reinitialize */
            shell_print(shell, "Reinitializing client due to inconsistent state");
            memset(&client, 0, sizeof(struct gopher_client));
            client_initialized = false;
        } else {
            return 0;
        }
    }
    
    ret = gopher_client_init(&client);
    if (ret < 0) {
        shell_error(shell, "Failed to initialize Gopher client: %d", ret);
        return ret;
    }
    
    /* Clear the buffer when initializing */
    memset(gopher_buffer, 0, sizeof(gopher_buffer));
    
    client_initialized = true;
    return 0;
}

/* Initialize networking */
static int init_networking(const struct shell *shell)
{
    struct net_if *iface;
    
    if (net_initialized) {
        return 0;
    }
    
    iface = net_if_get_default();
    if (!iface) {
        shell_error(shell, "No network interface found");
        return -ENODEV;
    }
    
    if (net_if_is_up(iface)) {
        shell_print(shell, "Network interface is up");
    } else {
        shell_print(shell, "Bringing up network interface...");
        net_if_up(iface);
    }
    
    if (IS_ENABLED(CONFIG_NET_DHCPV4)) {
        shell_print(shell, "Using DHCP for network configuration...");
        net_dhcpv4_start(iface);
    }
    
    net_initialized = true;
    return 0;
}

/* Display current IP address */
static int cmd_gopher_ip(const struct shell *shell, size_t argc, char **argv)
{
    struct net_if *iface;
    char addr_str[NET_IPV4_ADDR_LEN];
    struct net_if_ipv4 *ipv4;
    struct net_if_addr *unicast;
    int i;
    
    if (init_networking(shell) != 0) {
        return -ENODEV;
    }
    
    iface = net_if_get_default();
    if (!iface) {
        shell_error(shell, "No network interface found");
        return -ENODEV;
    }
    
    ipv4 = iface->config.ip.ipv4;
    if (!ipv4) {
        shell_error(shell, "No IPv4 configuration found");
        return -ENODATA;
    }
    
    shell_print(shell, "Network Interface Information:");
    shell_print(shell, "---------------------------");
    
    for (i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
        unicast = &ipv4->unicast[i];
        
        if (!unicast->is_used) {
            continue;
        }
        
        net_addr_ntop(AF_INET, &unicast->address.in_addr, addr_str, NET_IPV4_ADDR_LEN);
        shell_print(shell, "IPv4 Address: %s", addr_str);
    }
    
    net_addr_ntop(AF_INET, &ipv4->gw, addr_str, NET_IPV4_ADDR_LEN);
    shell_print(shell, "Gateway: %s", addr_str);
    
    /* NCS doesn't directly expose netmask as a member, so we'll skip it */
    shell_print(shell, "Netmask: (Not directly accessible in this SDK version)");

    
    return 0;
}

/* Connect to a Gopher server and automatically get root directory */
static int cmd_gopher_connect(const struct shell *shell, size_t argc, char **argv)
{
    const char *hostname;
    uint16_t port = GOPHER_DEFAULT_PORT;
    int ret;
    
    if (argc < 2) {
        shell_error(shell, "Usage: gopher connect <hostname> [port]");
        return -EINVAL;
    }
    
    if (init_networking(shell) != 0) {
        return -ENODEV;
    }
    
    /* We've completely rewritten gopher_connect to handle initialization safely */
    /* So we only need to make sure the client is initialized first time */
    if (!client_initialized) {
        /* First-time initialization only needed for client struct */
        memset(&client, 0, sizeof(struct gopher_client));
        client_initialized = true;
    }
    
    /* Clear buffer to avoid potential issues */
    memset(gopher_buffer, 0, sizeof(gopher_buffer));
    
    hostname = argv[1];
    
    if (argc >= 3) {
        port = atoi(argv[2]);
        if (port == 0) {
            port = GOPHER_DEFAULT_PORT;
        }
    }
    
    shell_print(shell, "Connecting to Gopher server %s:%d...", hostname, port);
    
    ret = gopher_connect(&client, hostname, port);
    if (ret < 0) {
        shell_error(shell, "Failed to connect to server: %d", ret);
        return ret;
    }
    
    shell_print(shell, "Connected to server successfully");
    
    /* Simpler connection with built-in timeout */
    shell_print(shell, "Fetching root directory...");
    
    ret = gopher_send_selector(&client, NULL, gopher_buffer, sizeof(gopher_buffer));
    
    if (ret < 0) {
        if (ret == -ETIMEDOUT) {
            shell_error(shell, "Connection to server timed out. Please check network connectivity.");
        } else if (ret == -ECONNREFUSED) {
            shell_error(shell, "Connection refused by server. The server may be down or not accepting connections.");
        } else if (ret == -EHOSTUNREACH) {
            shell_error(shell, "Host unreachable. Please check DNS settings and network routing.");
        } else {
            shell_error(shell, "Failed to get response from server: %d", ret);
        }
        
        /* Disconnect to clean up */
        client.connected = false;
        return ret;
    }
    
    /* Received response from server */
    
    /* Try to parse response as a directory listing */
    ret = gopher_parse_directory(&client, gopher_buffer);
    if (ret > 0) {
        /* Display directory header */
        shell_fprintf(shell, SHELL_NORMAL, "Gopher Directory: %s%s%s\n", 
                     COLOR_BLUE, client.hostname, COLOR_RESET);
        shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
        
        /* Display all items in their original order */
        int item_index = 0;
        for (int i = 0; i < client.item_count; i++) {
            /* Choose color based on item type */
            const char *color;
            const char *type_str;
            
            /* Special handling for info items */
            if (client.items[i].type == 'i') {
                /* Align with text after the type indicator with 10-char offset */
                shell_fprintf(shell, SHELL_NORMAL, "          %s%s%s\n", 
                             COLOR_GREEN, client.items[i].display_string, COLOR_RESET);
                continue;
            }
            
            /* For selectable items, increment the item index */
            item_index++;
            
            switch (client.items[i].type) {
                case GOPHER_TYPE_DIRECTORY:
                    color = COLOR_BLUE;
                    type_str = "DIR";
                    break;
                case GOPHER_TYPE_TEXT:
                    color = COLOR_WHITE;
                    type_str = "TXT";
                    break;
                case GOPHER_TYPE_SEARCH:
                    color = COLOR_GREEN;
                    type_str = "SRC";
                    break;
                case GOPHER_TYPE_IMAGE:
                case GOPHER_TYPE_GIF:
                    color = COLOR_MAGENTA;
                    type_str = "IMG";
                    break;
                case GOPHER_TYPE_BINARY:
                    color = COLOR_YELLOW;
                    type_str = "BIN";
                    break;
                case GOPHER_TYPE_ERROR:
                    color = COLOR_RED;
                    type_str = "ERR";
                    break;
                default:
                    color = COLOR_CYAN;
                    type_str = "UNK";
                    break;
            }
            
            /* Display item with line number and type */
            shell_fprintf(shell, SHELL_NORMAL, "%2d: %s[%s]%s %s\n", 
                         item_index, /* Starting from 1 for more intuitive numbering */
                         color, type_str, COLOR_RESET,
                         client.items[i].display_string);
        }
        
        shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
        shell_print(shell, "Use 'gopher view <index>' to view an item");
    } else {
        /* Check if this might be an image file */
        if (gopher_is_image((uint8_t *)gopher_buffer, ret)) {
            /* Display as image using ASCII art */
            shell_print(shell, "Detected image file, rendering as ASCII art...");
            
            /* Use default ASCII art configuration */
            ascii_art_config_t config = {
                .use_color = true,
                .use_dithering = true,
                .use_extended_chars = false,
                .color_mode = 8,
                .brightness = 1.0f,
                .contrast = 1.0f
            };
            
            /* Render the image */
            gopher_render_image(shell, (uint8_t *)gopher_buffer, ret, &config);
        } else {
            /* Display as text with proper formatting */
            shell_fprintf(shell, SHELL_NORMAL, "Gopher Text: %s%s%s\n", 
                        COLOR_BLUE, client.hostname, COLOR_RESET);
            shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
            
            /* Display text content in green for readability */
            char *line_start = gopher_buffer;
            char *line_end;
            char line_buffer[GOPHER_BUFFER_SIZE];
            
            /* Process each line */
            while (*line_start) {
                /* Find end of line */
                line_end = strstr(line_start, "\r\n");
                if (!line_end) {
                    /* Last line */
                    shell_fprintf(shell, SHELL_NORMAL, "%s%s%s\n", 
                                COLOR_GREEN, line_start, COLOR_RESET);
                    break;
                }
                
                /* Extract the line */
                int line_len = line_end - line_start;
                if (line_len >= sizeof(line_buffer)) {
                    line_len = sizeof(line_buffer) - 1;
                }
                memcpy(line_buffer, line_start, line_len);
                line_buffer[line_len] = '\0';
                
                /* Display the line */
                shell_fprintf(shell, SHELL_NORMAL, "%s%s%s\n", 
                            COLOR_GREEN, line_buffer, COLOR_RESET);
                
                /* Move to next line */
                line_start = line_end + 2;
            }
            
            shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
        }
    }
    
    return 0;
}

/* Send a selector and display the response */
static int cmd_gopher_get(const struct shell *shell, size_t argc, char **argv)
{
    const char *selector = NULL;
    int ret;
    
    if (ensure_client_initialized(shell) != 0) {
        return -EFAULT;
    }
    
    if (!client.connected) {
        shell_error(shell, "Not connected to a Gopher server. Use 'gopher connect' first.");
        return -ENOTCONN;
    }
    
    if (argc >= 2) {
        selector = argv[1];
    }
    
    shell_print(shell, "Requesting '%s' from %s:%d...", 
                selector ? selector : "(root)", client.hostname, client.port);
    
    ret = gopher_send_selector(&client, selector, gopher_buffer, sizeof(gopher_buffer));
    if (ret < 0) {
        shell_error(shell, "Failed to get response from server: %d", ret);
        return ret;
    }
    
    /* Try to parse response as a directory listing */
    ret = gopher_parse_directory(&client, gopher_buffer);
    if (ret > 0) {
        /* Display directory header */
        shell_fprintf(shell, SHELL_NORMAL, "Gopher Directory: %s%s%s\n", 
                     COLOR_BLUE, client.hostname, COLOR_RESET);
        shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
        
        /* Display all items in their original order */
        int item_index = 0;
        for (int i = 0; i < client.item_count; i++) {
            /* Choose color based on item type */
            const char *color;
            const char *type_str;
            
            /* Special handling for info items */
            if (client.items[i].type == 'i') {
                /* Align with text after the type indicator with 10-char offset */
                shell_fprintf(shell, SHELL_NORMAL, "          %s%s%s\n", 
                             COLOR_GREEN, client.items[i].display_string, COLOR_RESET);
                continue;
            }
            
            /* For selectable items, increment the item index */
            item_index++;
            
            switch (client.items[i].type) {
                case GOPHER_TYPE_DIRECTORY:
                    color = COLOR_BLUE;
                    type_str = "DIR";
                    break;
                case GOPHER_TYPE_TEXT:
                    color = COLOR_WHITE;
                    type_str = "TXT";
                    break;
                case GOPHER_TYPE_SEARCH:
                    color = COLOR_GREEN;
                    type_str = "SRC";
                    break;
                case GOPHER_TYPE_IMAGE:
                case GOPHER_TYPE_GIF:
                    color = COLOR_MAGENTA;
                    type_str = "IMG";
                    break;
                case GOPHER_TYPE_BINARY:
                    color = COLOR_YELLOW;
                    type_str = "BIN";
                    break;
                case GOPHER_TYPE_ERROR:
                    color = COLOR_RED;
                    type_str = "ERR";
                    break;
                default:
                    color = COLOR_CYAN;
                    type_str = "UNK";
                    break;
            }
            
            /* Display item with line number and type */
            shell_fprintf(shell, SHELL_NORMAL, "%2d: %s[%s]%s %s\n", 
                         item_index, /* Starting from 1 for more intuitive numbering */
                         color, type_str, COLOR_RESET,
                         client.items[i].display_string);
        }
        
        shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
        shell_print(shell, "Use 'gopher view <index>' to view an item");
    } else {
        /* Check if this might be an image file */
        if (gopher_is_image((uint8_t *)gopher_buffer, ret)) {
            /* Display as image using ASCII art */
            shell_print(shell, "Detected image file, rendering as ASCII art...");
            
            /* Use default ASCII art configuration */
            ascii_art_config_t config = {
                .use_color = true,
                .use_dithering = true,
                .use_extended_chars = false,
                .color_mode = 8,
                .brightness = 1.0f,
                .contrast = 1.0f
            };
            
            /* Render the image */
            gopher_render_image(shell, (uint8_t *)gopher_buffer, ret, &config);
        } else {
            /* Display as text with proper formatting */
            shell_fprintf(shell, SHELL_NORMAL, "Gopher Text: %s%s%s\n", 
                        COLOR_BLUE, client.hostname, COLOR_RESET);
            shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
            
            /* Display text content in green for readability */
            char *line_start = gopher_buffer;
            char *line_end;
            char line_buffer[GOPHER_BUFFER_SIZE];
            
            /* Process each line */
            while (*line_start) {
                /* Find end of line */
                line_end = strstr(line_start, "\r\n");
                if (!line_end) {
                    /* Last line */
                    shell_fprintf(shell, SHELL_NORMAL, "%s%s%s\n", 
                                COLOR_GREEN, line_start, COLOR_RESET);
                    break;
                }
                
                /* Extract the line */
                int line_len = line_end - line_start;
                if (line_len >= sizeof(line_buffer)) {
                    line_len = sizeof(line_buffer) - 1;
                }
                memcpy(line_buffer, line_start, line_len);
                line_buffer[line_len] = '\0';
                
                /* Display the line */
                shell_fprintf(shell, SHELL_NORMAL, "%s%s%s\n", 
                            COLOR_GREEN, line_buffer, COLOR_RESET);
                
                /* Move to next line */
                line_start = line_end + 2;
            }
            
            shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
        }
    }
    
    return 0;
}

/* View a specific item from the directory listing */
static int cmd_gopher_view(const struct shell *shell, size_t argc, char **argv)
{
    int index;
    int ret;
    char *selector;
    char *hostname;
    uint16_t port;
    
    if (argc < 2) {
        shell_error(shell, "Usage: gopher view <index>");
        return -EINVAL;
    }
    
    if (ensure_client_initialized(shell) != 0) {
        return -EFAULT;
    }
    
    if (!client.connected) {
        shell_error(shell, "Not connected to a Gopher server. Use 'gopher connect' first.");
        return -ENOTCONN;
    }
    
    if (client.item_count == 0) {
        shell_error(shell, "No items in current directory. Use 'gopher get' first.");
        return -ENODATA;
    }
    
    /* Get the index from user input */
    index = atoi(argv[1]);
    if (index < 1) {
        shell_error(shell, "Invalid item index. Must be between 1 and %d", client.item_count - gopher_count_info_items(&client));
        return -EINVAL;
    }
    index--; /* Convert from 1-based display index to 0-based array index */
    
    /* Count the number of info items to find the real index */
    int real_index = 0;
    int info_count = 0;
    for (int i = 0; i < client.item_count; i++) {
        if (client.items[i].type == 'i') {
            info_count++;
            continue;
        }
        if (real_index == index) {
            index = i;
            break;
        }
        real_index++;
    }
    
    /* Debug info about indices */
    shell_print(shell, "DEBUG: User requested index %d, translated to index %d of %d items (after %d info items)",
              atoi(argv[1]), index, client.item_count, info_count);

    /* Check if the index is valid after adjusting for info items */
    if (index < 0 || index >= client.item_count) {
        shell_error(shell, "Invalid item index. Must be between 1 and %d", 
                   client.item_count - info_count);
        return -EINVAL;
    }
    
    selector = client.items[index].selector;
    hostname = client.items[index].hostname;
    port = client.items[index].port;
    
    /* Debug item details */
    shell_print(shell, "DEBUG: Selected item - Type: %c, Hostname: %s, Port: %d, Selector: %s",
              client.items[index].type, hostname, port, selector);
    
    /* Check if this item is on a different server */
    if (strcmp(hostname, client.hostname) != 0 || port != client.port) {
        shell_print(shell, "Item is on a different server (%s:%d). Connecting...", 
                    hostname, port);
        
        /* Reset client state before connecting to a different server */
        /* Keep history intact but reset other state */
        char history_backup[10][GOPHER_MAX_SELECTOR_LEN];
        int history_pos_backup = client.history_pos;
        int history_count_backup = client.history_count;
        
        /* Backup navigation history */
        memcpy(history_backup, client.history, sizeof(client.history));
        
        /* Reset client state */
        memset(&client, 0, sizeof(struct gopher_client));
        client.port = GOPHER_DEFAULT_PORT;
        client.connected = false;
        client.item_count = 0;
        
        /* Restore navigation history */
        memcpy(client.history, history_backup, sizeof(client.history));
        client.history_pos = history_pos_backup;
        client.history_count = history_count_backup;
        
        /* Clear buffer */
        memset(gopher_buffer, 0, sizeof(gopher_buffer));
        
        ret = gopher_connect(&client, hostname, port);
        if (ret < 0) {
            shell_error(shell, "Failed to connect to server %s:%d: %d", 
                        hostname, port, ret);
            return ret;
        }
    }
    
    /* Handle special item types */
    switch (client.items[index].type) {
        case GOPHER_TYPE_TELNET:
        case GOPHER_TYPE_TN3270:
            shell_print(shell, "Telnet sessions are not supported in this client");
            return -ENOTSUP;
            
        case GOPHER_TYPE_BINARY:
        case GOPHER_TYPE_DOS:
        case GOPHER_TYPE_BINHEX:
        case GOPHER_TYPE_UUENCODED:
            shell_print(shell, "Binary files are not supported in this client");
            return -ENOTSUP;
            
        case GOPHER_TYPE_GIF:
        case GOPHER_TYPE_IMAGE:
            /* These are now supported with the image renderer */
            shell_print(shell, "Fetching image file for rendering...");
            break;
            
        default:
            /* Continue with normal retrieval */
            break;
    }
    
    /* Display original user-requested index, not internal adjusted index */
    shell_print(shell, "Requesting item %d: '%s' (%c) from %s:%d...", 
                atoi(argv[1]), /* Use the user's input index */
                client.items[index].display_string, 
                client.items[index].type,
                client.hostname, 
                client.port);
                
    /* For images, remove detailed logging to avoid build errors */
    
    ret = gopher_send_selector(&client, selector, gopher_buffer, sizeof(gopher_buffer));
    if (ret < 0) {
        shell_error(shell, "Failed to get response from server: %d", ret);
        return ret;
    }
    
    /* For directory items, parse and display the listing */
    if (client.items[index].type == GOPHER_TYPE_DIRECTORY) {
        ret = gopher_parse_directory(&client, gopher_buffer);
        if (ret > 0) {
            /* Display directory header */
            shell_fprintf(shell, SHELL_NORMAL, "Gopher Directory: %s%s%s\n", 
                        COLOR_BLUE, client.hostname, COLOR_RESET);
            shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
            
            /* Display all items in their original order */
            int item_index = 0;
            for (int i = 0; i < client.item_count; i++) {
                /* Choose color based on item type */
                const char *color;
                const char *type_str;
                
                /* Special handling for info items */
                if (client.items[i].type == 'i') {
                    /* Align with text after the type indicator */
                    shell_fprintf(shell, SHELL_NORMAL, "    %s%s%s\n", 
                                COLOR_GREEN, client.items[i].display_string, COLOR_RESET);
                    continue;
                }
                
                /* For selectable items, increment the item index */
                item_index++;
                
                switch (client.items[i].type) {
                    case GOPHER_TYPE_DIRECTORY:
                        color = COLOR_BLUE;
                        type_str = "DIR";
                        break;
                    case GOPHER_TYPE_TEXT:
                        color = COLOR_WHITE;
                        type_str = "TXT";
                        break;
                    case GOPHER_TYPE_SEARCH:
                        color = COLOR_GREEN;
                        type_str = "SRC";
                        break;
                    case GOPHER_TYPE_IMAGE:
                    case GOPHER_TYPE_GIF:
                        color = COLOR_MAGENTA;
                        type_str = "IMG";
                        break;
                    case GOPHER_TYPE_BINARY:
                        color = COLOR_YELLOW;
                        type_str = "BIN";
                        break;
                    case GOPHER_TYPE_ERROR:
                        color = COLOR_RED;
                        type_str = "ERR";
                        break;
                    default:
                        color = COLOR_CYAN;
                        type_str = "UNK";
                        break;
                }
                
                /* Display item with line number and type */
                shell_fprintf(shell, SHELL_NORMAL, "%2d: %s[%s]%s %s\n", 
                            item_index,
                            color, type_str, COLOR_RESET,
                            client.items[i].display_string);
            }
            
            shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
            shell_print(shell, "Use 'gopher view <index>' to view an item");
        } else {
            shell_error(shell, "Failed to parse directory listing or empty directory");
            shell_print(shell, "Raw response:");
            shell_print(shell, "-------------");
            shell_print(shell, "%s", gopher_buffer);
        }
    } else {
        /* Check if this might be an image file */
        if (gopher_is_image((uint8_t *)gopher_buffer, ret)) {
            /* Display as image using ASCII art */
            shell_print(shell, "Detected image file, rendering as ASCII art...");
            
            /* Use default ASCII art configuration */
            ascii_art_config_t config = {
                .use_color = true,
                .use_dithering = true,
                .use_extended_chars = false,
                .color_mode = 8,
                .brightness = 1.0f,
                .contrast = 1.0f
            };
            
            /* Render the image */
            gopher_render_image(shell, (uint8_t *)gopher_buffer, ret, &config);
        } else {
            /* Display as text with proper formatting */
            shell_fprintf(shell, SHELL_NORMAL, "Gopher Text: %s%s%s\n", 
                        COLOR_BLUE, client.hostname, COLOR_RESET);
            shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
            
            /* Display text content in green for readability */
            char *line_start = gopher_buffer;
            char *line_end;
            char line_buffer[GOPHER_BUFFER_SIZE];
            
            /* Process each line */
            while (*line_start) {
                /* Find end of line */
                line_end = strstr(line_start, "\r\n");
                if (!line_end) {
                    /* Last line */
                    shell_fprintf(shell, SHELL_NORMAL, "%s%s%s\n", 
                                COLOR_GREEN, line_start, COLOR_RESET);
                    break;
                }
                
                /* Extract the line */
                int line_len = line_end - line_start;
                if (line_len >= sizeof(line_buffer)) {
                    line_len = sizeof(line_buffer) - 1;
                }
                memcpy(line_buffer, line_start, line_len);
                line_buffer[line_len] = '\0';
                
                /* Display the line */
                shell_fprintf(shell, SHELL_NORMAL, "%s%s%s\n", 
                            COLOR_GREEN, line_buffer, COLOR_RESET);
                
                /* Move to next line */
                line_start = line_end + 2;
            }
            
            shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
        }
    }
    
    return 0;
}

/* Navigate back in history */
static int cmd_gopher_back(const struct shell *shell, size_t argc, char **argv)
{
    int ret;
    
    if (ensure_client_initialized(shell) != 0) {
        return -EFAULT;
    }
    
    if (!client.connected) {
        shell_error(shell, "Not connected to a Gopher server. Use 'gopher connect' first.");
        return -ENOTCONN;
    }
    
    if (client.history_count <= 1) {
        shell_error(shell, "No previous items in history");
        return -ENODATA;
    }
    
    /* Move back in history */
    client.history_pos = (client.history_pos + 10 - 1) % 10;
    
    shell_print(shell, "Navigating back to: '%s'", client.history[client.history_pos]);
    
    ret = gopher_send_selector(&client, client.history[client.history_pos], 
                              gopher_buffer, sizeof(gopher_buffer));
    if (ret < 0) {
        shell_error(shell, "Failed to get response from server: %d", ret);
        return ret;
    }
    
    /* Determine if this is a directory or a text file */
    ret = gopher_parse_directory(&client, gopher_buffer);
    if (ret > 0) {
        /* Display directory header */
        shell_fprintf(shell, SHELL_NORMAL, "Gopher Directory: %s%s%s\n", 
                    COLOR_BLUE, client.hostname, COLOR_RESET);
        shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
        
        /* Display all items in their original order */
        int item_index = 0;
        for (int i = 0; i < client.item_count; i++) {
            /* Choose color based on item type */
            const char *color;
            const char *type_str;
            
            /* Special handling for info items */
            if (client.items[i].type == 'i') {
                /* Align with text after the type indicator with 10-char offset */
                shell_fprintf(shell, SHELL_NORMAL, "          %s%s%s\n", 
                             COLOR_GREEN, client.items[i].display_string, COLOR_RESET);
                continue;
            }
            
            /* For selectable items, increment the item index */
            item_index++;
            
            switch (client.items[i].type) {
                case GOPHER_TYPE_DIRECTORY:
                    color = COLOR_BLUE;
                    type_str = "DIR";
                    break;
                case GOPHER_TYPE_TEXT:
                    color = COLOR_WHITE;
                    type_str = "TXT";
                    break;
                case GOPHER_TYPE_SEARCH:
                    color = COLOR_GREEN;
                    type_str = "SRC";
                    break;
                case GOPHER_TYPE_IMAGE:
                case GOPHER_TYPE_GIF:
                    color = COLOR_MAGENTA;
                    type_str = "IMG";
                    break;
                case GOPHER_TYPE_BINARY:
                    color = COLOR_YELLOW;
                    type_str = "BIN";
                    break;
                case GOPHER_TYPE_ERROR:
                    color = COLOR_RED;
                    type_str = "ERR";
                    break;
                default:
                    color = COLOR_CYAN;
                    type_str = "UNK";
                    break;
            }
            
            /* Display item with line number and type */
            shell_fprintf(shell, SHELL_NORMAL, "%2d: %s[%s]%s %s\n", 
                         item_index, /* Starting from 1 for more intuitive numbering */
                         color, type_str, COLOR_RESET,
                         client.items[i].display_string);
        }
        
        shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
        shell_print(shell, "Use 'gopher view <index>' to view an item");
    } else {
        /* Check if this might be an image file */
        if (gopher_is_image((uint8_t *)gopher_buffer, ret)) {
            
            /* Display as image using ASCII art */
            shell_print(shell, "Rendering image as ASCII art...");
            
            /* Use default ASCII art configuration */
            ascii_art_config_t config = {
                .use_color = true,
                .use_dithering = true,
                .use_extended_chars = false,
                .color_mode = 8,
                .brightness = 1.0f,
                .contrast = 1.0f
            };
            
            /* Render the image */
            gopher_render_image(shell, (uint8_t *)gopher_buffer, ret, &config);
            
        } else {
            /* Display as text with proper formatting */
            shell_fprintf(shell, SHELL_NORMAL, "Gopher Text: %s%s%s\n", 
                        COLOR_BLUE, client.hostname, COLOR_RESET);
            shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
            
            /* Display text content in green for readability */
            char *line_start = gopher_buffer;
            char *line_end;
            char line_buffer[GOPHER_BUFFER_SIZE];
            
            /* Process each line */
            while (*line_start) {
                /* Find end of line */
                line_end = strstr(line_start, "\r\n");
                if (!line_end) {
                    /* Last line */
                    shell_fprintf(shell, SHELL_NORMAL, "%s%s%s\n", 
                                COLOR_GREEN, line_start, COLOR_RESET);
                    break;
                }
                
                /* Extract the line */
                int line_len = line_end - line_start;
                if (line_len >= sizeof(line_buffer)) {
                    line_len = sizeof(line_buffer) - 1;
                }
                memcpy(line_buffer, line_start, line_len);
                line_buffer[line_len] = '\0';
                
                /* Display the line */
                shell_fprintf(shell, SHELL_NORMAL, "%s%s%s\n", 
                            COLOR_GREEN, line_buffer, COLOR_RESET);
                
                /* Move to next line */
                line_start = line_end + 2;
            }
            
            shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
        }
    }
    
    return 0;
}

/* Display search interface */
static int cmd_gopher_search(const struct shell *shell, size_t argc, char **argv)
{
    int index;
    int ret;
    char search_selector[GOPHER_MAX_SELECTOR_LEN];
    
    if (argc < 3) {
        shell_error(shell, "Usage: gopher search <index> <search_string>");
        return -EINVAL;
    }
    
    if (ensure_client_initialized(shell) != 0) {
        return -EFAULT;
    }
    
    if (!client.connected) {
        shell_error(shell, "Not connected to a Gopher server. Use 'gopher connect' first.");
        return -ENOTCONN;
    }
    
    if (client.item_count == 0) {
        shell_error(shell, "No items in current directory. Use 'gopher get' first.");
        return -ENODATA;
    }
    
    /* Get the index from user input */
    index = atoi(argv[1]);
    if (index < 1) {
        shell_error(shell, "Invalid item index. Must be between 1 and %d", client.item_count - gopher_count_info_items(&client));
        return -EINVAL;
    }
    index--; /* Convert from 1-based display index to 0-based array index */
    
    /* Count the number of info items to find the real index */
    int real_index = 0;
    int info_count = 0;
    for (int i = 0; i < client.item_count; i++) {
        if (client.items[i].type == 'i') {
            info_count++;
            continue;
        }
        if (real_index == index) {
            index = i;
            break;
        }
        real_index++;
    }
    
    /* Check if item is a search server */
    if (client.items[index].type != GOPHER_TYPE_SEARCH) {
        shell_error(shell, "Item %d is not a search server", index + 1);
        return -EINVAL;
    }
    
    /* Construct search selector: selector<TAB>search_string */
    snprintf(search_selector, sizeof(search_selector), "%s\t%s", 
             client.items[index].selector, argv[2]);
    
    /* Check if this item is on a different server */
    if (strcmp(client.items[index].hostname, client.hostname) != 0 || 
        client.items[index].port != client.port) {
        
        shell_print(shell, "Search server is on %s:%d. Connecting...", 
                    client.items[index].hostname, client.items[index].port);
        
        /* Reset client state before connecting to a different server */
        /* Keep history intact but reset other state */
        char history_backup[10][GOPHER_MAX_SELECTOR_LEN];
        int history_pos_backup = client.history_pos;
        int history_count_backup = client.history_count;
        
        /* Backup navigation history */
        memcpy(history_backup, client.history, sizeof(client.history));
        
        /* Reset client state */
        memset(&client, 0, sizeof(struct gopher_client));
        client.port = GOPHER_DEFAULT_PORT;
        client.connected = false;
        client.item_count = 0;
        
        /* Restore navigation history */
        memcpy(client.history, history_backup, sizeof(client.history));
        client.history_pos = history_pos_backup;
        client.history_count = history_count_backup;
        
        /* Clear buffer */
        memset(gopher_buffer, 0, sizeof(gopher_buffer));
        
        ret = gopher_connect(&client, client.items[index].hostname, 
                           client.items[index].port);
        if (ret < 0) {
            shell_error(shell, "Failed to connect to server: %d", ret);
            return ret;
        }
    }
    
    shell_print(shell, "Searching for '%s'...", argv[2]);
    
    ret = gopher_send_selector(&client, search_selector, gopher_buffer, sizeof(gopher_buffer));
    if (ret < 0) {
        shell_error(shell, "Failed to get search results: %d", ret);
        return ret;
    }
    
    /* Parse search results as directory listing */
    ret = gopher_parse_directory(&client, gopher_buffer);
    if (ret > 0) {
        /* Display search results header */
        shell_fprintf(shell, SHELL_NORMAL, "Search Results: %s%s%s\n", 
                    COLOR_BLUE, client.hostname, COLOR_RESET);
        shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
        shell_fprintf(shell, SHELL_NORMAL, "%sSearch query: %s%s\n\n", 
                    COLOR_GREEN, argv[2], COLOR_RESET);
        
        /* Display all items in their original order */
        int item_index = 0;
        for (int i = 0; i < client.item_count; i++) {
            /* Choose color based on item type */
            const char *color;
            const char *type_str;
            
            /* Special handling for info items */
            if (client.items[i].type == 'i') {
                /* Align with text after the type indicator with 10-char offset */
                shell_fprintf(shell, SHELL_NORMAL, "          %s%s%s\n", 
                             COLOR_GREEN, client.items[i].display_string, COLOR_RESET);
                continue;
            }
            
            /* For selectable items, increment the item index */
            item_index++;
            
            switch (client.items[i].type) {
                case GOPHER_TYPE_DIRECTORY:
                    color = COLOR_BLUE;
                    type_str = "DIR";
                    break;
                case GOPHER_TYPE_TEXT:
                    color = COLOR_WHITE;
                    type_str = "TXT";
                    break;
                case GOPHER_TYPE_SEARCH:
                    color = COLOR_GREEN;
                    type_str = "SRC";
                    break;
                case GOPHER_TYPE_IMAGE:
                case GOPHER_TYPE_GIF:
                    color = COLOR_MAGENTA;
                    type_str = "IMG";
                    break;
                case GOPHER_TYPE_BINARY:
                    color = COLOR_YELLOW;
                    type_str = "BIN";
                    break;
                case GOPHER_TYPE_ERROR:
                    color = COLOR_RED;
                    type_str = "ERR";
                    break;
                default:
                    color = COLOR_CYAN;
                    type_str = "UNK";
                    break;
            }
            
            /* Display item with line number and type */
            shell_fprintf(shell, SHELL_NORMAL, "%2d: %s[%s]%s %s\n", 
                         item_index, /* Starting from 1 for more intuitive numbering */
                         color, type_str, COLOR_RESET,
                         client.items[i].display_string);
        }
        
        shell_fprintf(shell, SHELL_NORMAL, "---------------------------------------------\n");
        shell_print(shell, "Use 'gopher view <index>' to view a result");
    } else {
        shell_error(shell, "No search results found or error parsing results");
    }
    
    return 0;
}

/* These helper functions are now defined at the top of the file */

/* Display help information */
static int cmd_gopher_help(const struct shell *shell, size_t argc, char **argv)
{
    shell_print(shell, "Gopher Client Commands:");
    shell_print(shell, "----------------------");
    shell_print(shell, "NOTE: All commands can be used with 'g' instead of 'gopher' (e.g., 'g connect')");
    shell_print(shell, "");
    shell_print(shell, "gopher ip - Display network information");
    shell_print(shell, "gopher connect <host> [port] - Connect to a Gopher server and get root directory");
    shell_print(shell, "gopher get [selector] - Request a document or directory");
    shell_print(shell, "gopher view <index> - View an item from the directory");
    shell_print(shell, "gopher back - Navigate back to previous item");
    shell_print(shell, "gopher search <index> <search_string> - Search using a search server");
    shell_print(shell, "gopher help - Display this help message");
    shell_print(shell, "");
    shell_print(shell, "Examples:");
    shell_print(shell, "  g connect gopher.floodgap.com - Connect to Floodgap's Gopher server");
    shell_print(shell, "  g view 1 - View the first item in the directory");
    shell_print(shell, "  g 1      - Shortcut for 'g view 1' to view the first item");
    shell_print(shell, "  g back   - Navigate back to the previous item");
    shell_print(shell, "");
    shell_print(shell, "Note: Image files are automatically detected and rendered as ASCII art.");
    return 0;
}

/* Define subcommands for the gopher command */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_gopher,
    SHELL_CMD(ip, NULL, "Display network information", cmd_gopher_ip),
    SHELL_CMD(connect, NULL, "Connect to a Gopher server and fetch root directory", cmd_gopher_connect),
    SHELL_CMD(get, NULL, "Request a document or directory", cmd_gopher_get),
    SHELL_CMD(view, NULL, "View an item from the directory (use item number)", cmd_gopher_view),
    SHELL_CMD(back, NULL, "Navigate back to previous item", cmd_gopher_back),
    SHELL_CMD(search, NULL, "Search using a search server", cmd_gopher_search),
    SHELL_CMD(help, NULL, "Display help information", cmd_gopher_help),
    SHELL_SUBCMD_SET_END
);

/* Special handler for 'g' command to support 'g <number>' shortcut */
static int cmd_g_handler(const struct shell *shell, size_t argc, char **argv)
{
    /* Check for empty command */
    if (argc <= 1) {
        cmd_gopher_help(shell, argc, argv);
        return 0;
    }
    
    /* Check if this might be a direct item access with 'g <number>' */
    if (isdigit((unsigned char)argv[1][0])) {
        /* This looks like a number, treat it as 'g view <number>' */
        char *view_args[3] = {"view", argv[1], NULL};
        return cmd_gopher_view(shell, 2, view_args);
    }

    /* Manual mapping of commands to handlers since shell_cmd_get isn't available */
    if (strcmp(argv[1], "ip") == 0) {
        return cmd_gopher_ip(shell, argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "connect") == 0) {
        return cmd_gopher_connect(shell, argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "get") == 0) {
        return cmd_gopher_get(shell, argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "view") == 0) {
        return cmd_gopher_view(shell, argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "back") == 0) {
        return cmd_gopher_back(shell, argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "search") == 0) {
        return cmd_gopher_search(shell, argc - 1, &argv[1]);
    } else if (strcmp(argv[1], "help") == 0) {
        return cmd_gopher_help(shell, argc - 1, &argv[1]);
    } else {
        shell_error(shell, "Unknown command: %s", argv[1]);
        cmd_gopher_help(shell, argc, argv);
        return -EINVAL;
    }
}

/* Register the main command and its alias */
SHELL_CMD_REGISTER(gopher, &sub_gopher, "Gopher client commands", cmd_gopher_help);
SHELL_CMD_REGISTER(g, NULL, "Gopher client commands (alias with 'g <n>' shortcut)", cmd_g_handler);

/* Initialize the Gopher shell module */
static int gopher_shell_init(void)
{
    int ret = gopher_client_init(&client);
    if (ret == 0) {
        client_initialized = true;
    }
    return ret;
}

SYS_INIT(gopher_shell_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
