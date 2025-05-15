/*
 * Copyright (c) 2023 Gopher Client
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "gopher_client.h"

LOG_MODULE_REGISTER(gopher_client, LOG_LEVEL_ERR);

/* Initialize the Gopher client */
int gopher_client_init(struct gopher_client *client)
{
    if (client == NULL) {
        return -EINVAL;
    }

    memset(client, 0, sizeof(struct gopher_client));
    client->port = GOPHER_DEFAULT_PORT;
    client->connected = false;
    client->item_count = 0;
    client->history_pos = 0;
    client->history_count = 0;

    return 0;
}

/* Connect to a Gopher server - simplified memory safe version */
int gopher_connect(struct gopher_client *client, const char *hostname, uint16_t port)
{
    /* Critical safety check */
    if (client == NULL || hostname == NULL) {
        return -EINVAL;
    }
    
    /* Safety: completely reset client state */
    memset(client, 0, sizeof(struct gopher_client));
    
    /* Set port */
    client->port = (port == 0) ? GOPHER_DEFAULT_PORT : port;
    
    /* Store hostname with safe strncpy */
    strncpy(client->hostname, hostname, GOPHER_MAX_HOSTNAME_LEN - 1);
    client->hostname[GOPHER_MAX_HOSTNAME_LEN - 1] = '\0';
    
    /* Set connected flag */
    client->connected = true;
    
    return 0;
}

/* Send a selector string to the server and receive the response */
int gopher_send_selector(struct gopher_client *client, const char *selector, 
                         char *buffer, size_t buffer_size)
{
    int ret = 0;
    int sock = -1;
    int total_received = 0;
    struct sockaddr_in server;
    char port_str[8];
    
    /* Safety checks - fail fast */
    if (client == NULL || buffer == NULL || buffer_size == 0) {
        return -EINVAL;
    }
    
    if (!client->connected || client->hostname[0] == '\0') {
        return -ENOTCONN;
    }
    
    /* Clear buffer completely */
    memset(buffer, 0, buffer_size);
    
    /* Set up server address structure */
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(client->port);
    
    /* Convert port to string for getaddrinfo */
    snprintf(port_str, sizeof(port_str), "%u", client->port);
    
    /* Try hostname resolution with getaddrinfo */
    struct zsock_addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    /* First try direct IP address parsing */
    if (zsock_inet_pton(AF_INET, client->hostname, &server.sin_addr) != 1) {
        /* Not an IP address, try DNS resolution */
        int err = zsock_getaddrinfo(client->hostname, NULL, &hints, &result);
        if (err != 0) {
            return -EHOSTUNREACH;
        }
        
        /* Copy the resolved address */
        memcpy(&server.sin_addr, 
               &((struct sockaddr_in *)result->ai_addr)->sin_addr,
               sizeof(struct in_addr));
               
        zsock_freeaddrinfo(result);
    }
    
    /* Create socket */
    sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return -ESOCKTNOSUPPORT;
    }
    
    /* Set reasonable timeouts */
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    
    zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    /* Connect to server */
    int err = zsock_connect(sock, (struct sockaddr *)&server, sizeof(server));
    if (err < 0) {
        zsock_close(sock);
        return -errno;  /* Return the actual error code */
    }
    
    /* Prepare request */
    char request[128];
    memset(request, 0, sizeof(request));
    
    if (selector == NULL) {
        /* Empty selector */
        strcpy(request, "\r\n");
    } else {
        /* Add selector with CRLF */
        size_t req_len = strlen(selector);
        if (req_len > sizeof(request) - 3) {
            req_len = sizeof(request) - 3;
        }
        
        memcpy(request, selector, req_len);
        request[req_len] = '\r';
        request[req_len+1] = '\n';
        request[req_len+2] = '\0';
    }
    
    /* Send request */
    err = zsock_send(sock, request, strlen(request), 0);
    if (err < 0) {
        zsock_close(sock);
        return -errno;  /* Return the actual error code */
    }
    
    /* Receive response */
    char recv_buffer[128];
    int bytes_read;
    
    /* Receive data in smaller chunks to reduce stack usage */
    int recv_attempts = 0;
    while (recv_attempts < 3) { /* Try a few times */
        memset(recv_buffer, 0, sizeof(recv_buffer));
        bytes_read = zsock_recv(sock, recv_buffer, sizeof(recv_buffer) - 1, 0);
        
        if (bytes_read < 0) {
            recv_attempts++;
            k_sleep(K_MSEC(500)); /* Wait a bit before retrying */
            continue;
        } else if (bytes_read == 0) {
            /* Connection closed by server */
            break;
        }
        
        /* Don't overflow the buffer */
        if (total_received + bytes_read >= buffer_size) {
            bytes_read = buffer_size - total_received - 1;
            if (bytes_read <= 0) {
                break;
            }
        }
        
        /* Copy data to output buffer */
        memcpy(buffer + total_received, recv_buffer, bytes_read);
        total_received += bytes_read;
        buffer[total_received] = '\0';
        
        /* Check if we've filled the buffer */
        if (total_received >= buffer_size - 1) {
            break;
        }
        
        /* If we received data, reset retry counter */
        recv_attempts = 0;
    }
    
    /* Close the socket when done */
    zsock_close(sock);
    
    /* Update history if we got data */
    if (total_received > 0) {
        /* Add to history */
        if (client->history_count < 10) {
            client->history_count++;
        }
        
        /* Move to the next position */
        client->history_pos = (client->history_pos + 1) % 10;
        
        /* Store selector */
        if (selector != NULL) {
            memset(client->history[client->history_pos], 0, GOPHER_MAX_SELECTOR_LEN);
            strncpy(client->history[client->history_pos], selector, GOPHER_MAX_SELECTOR_LEN - 1);
        } else {
            client->history[client->history_pos][0] = '\0';
        }
    }
    
    return total_received;
}

/* Basic history update function - can be used for direct history management */
int gopher_update_history(struct gopher_client *client, const char *selector)
{
    if (client == NULL) {
        return -EINVAL;
    }
    
    /* Update history */
    if (client->history_count < 10) {
        client->history_count++;
    }
    
    /* Move to next position */
    client->history_pos = (client->history_pos + 1) % 10;
    
    /* Store selector */
    memset(client->history[client->history_pos], 0, GOPHER_MAX_SELECTOR_LEN);
    if (selector != NULL) {
        strncpy(client->history[client->history_pos], selector, GOPHER_MAX_SELECTOR_LEN - 1);
    }
    
    return 0;
}

/* Parse a directory listing with improved safety */
int gopher_parse_directory(struct gopher_client *client, const char *buffer)
{
    int count = 0;
    const char *line_start;
    const char *line_end;
    const char *field_start;
    const char *field_end;
    const char *buffer_end;
    size_t buffer_len;
    
    /* Critical safety check */
    if (client == NULL || buffer == NULL) {
        return -EINVAL;
    }
    
    /* Get buffer length for bounds checking */
    buffer_len = strlen(buffer);
    if (buffer_len == 0) {
        return 0;
    }
    
    /* Calculate buffer end pointer for bounds checking */
    buffer_end = buffer + buffer_len;
    
    /* Start with clean slate */
    client->item_count = 0;
    
    /* More safety: clear all items first with memset */
    memset(client->items, 0, sizeof(client->items));
    
    /* Initialize line_start to avoid potential NULL pointer */
    line_start = buffer;
    
    /* Check if this is a text file or a directory listing */
    /* If the first character isn't a valid Gopher item type followed by a display string and a tab,
       it's probably a text file, not a directory listing */
    bool is_dir_listing = false;
    
    /* First check if buffer has at least one character */
    if (buffer_len > 0) {
        char first_char = buffer[0];
        /* Check if the first character is a valid item type according to RFC 1436 */
        if (first_char == GOPHER_TYPE_TEXT ||
            first_char == GOPHER_TYPE_DIRECTORY ||
            first_char == GOPHER_TYPE_CSO ||
            first_char == GOPHER_TYPE_ERROR ||
            first_char == GOPHER_TYPE_BINHEX ||
            first_char == GOPHER_TYPE_DOS ||
            first_char == GOPHER_TYPE_UUENCODED ||
            first_char == GOPHER_TYPE_SEARCH ||
            first_char == GOPHER_TYPE_TELNET ||
            first_char == GOPHER_TYPE_BINARY ||
            first_char == GOPHER_TYPE_REDUNDANT ||
            first_char == GOPHER_TYPE_TN3270 ||
            first_char == GOPHER_TYPE_GIF ||
            first_char == GOPHER_TYPE_IMAGE ||
            first_char == 'i') {
            /* Look for a tab character which separates fields in the first line */
            if (strchr(buffer, '\t') != NULL) {
                is_dir_listing = true;
            }
        }
    }
    
    /* If this doesn't look like a directory listing, return 0 to indicate 
       it should be treated as a text file */
    if (!is_dir_listing) {
        return 0;
    }
    
    /* Parse directory listing */
    
    /* Process each line with careful bounds checking */
    while (line_start < buffer_end && count < GOPHER_MAX_DIR_ITEMS) {
        /* Verify line_start is within bounds */
        if (line_start < buffer || line_start >= buffer_end) {
            break;
        }
        
        /* Skip empty lines or if we've reached the end */
        if (*line_start == '\0') {
            break;
        }
        
        /* Find end of line with bounds checking */
        line_end = buffer_end;  /* Default to end of buffer */
        const char *crlf = memmem(line_start, buffer_end - line_start, "\r\n", 2);
        if (crlf != NULL && crlf < buffer_end) {
            line_end = crlf;
        } else {
            /* No CRLF found, this is the last line */
            line_end = buffer_end;
            /* If we're at the end of buffer without CRLF, we can still process this line */
        }
        
        /* Check if this is the terminating period */
        if (line_start[0] == '.' && line_end == line_start + 1) {
            break;
        }
        
        /* Extra safety: check if we have at least one character in the line */
        if (line_end <= line_start) {
            /* Empty line, move to next */
            if (line_end + 2 <= buffer_end) {
                line_start = line_end + 2;
            } else {
                /* We've reached the end */
                break;
            }
            continue;
        }
        
        /* Get item type (first character) with bounds check */
        client->items[count].type = line_start[0];
        
        /* Skip type character */
        field_start = line_start + 1;
        if (field_start >= buffer_end) {
            break;  /* Unexpected end of buffer */
        }
        
        /* Get display string (first field) */
        field_end = memchr(field_start, '\t', line_end - field_start);
        if (!field_end || field_end > line_end) {
            /* For info items (type 'i'), we can tolerate missing tabs 
               This makes the client more compatible with non-standard servers */
            if (client->items[count].type == 'i') {
                /* Use the whole line as the display string */
                size_t len = line_end - field_start;
                if (len >= GOPHER_MAX_SELECTOR_LEN) {
                    len = GOPHER_MAX_SELECTOR_LEN - 1;
                }
                
                /* Safer strncpy instead of memcpy */
                memset(client->items[count].display_string, 0, GOPHER_MAX_SELECTOR_LEN);
                if (len > 0) {
                    strncpy(client->items[count].display_string, field_start, len);
                }
                client->items[count].display_string[len] = '\0';
                
                /* Use empty values for other fields */
                client->items[count].selector[0] = '\0';
                /* Make sure hostname doesn't exceed buffer */
                strncpy(client->items[count].hostname, client->hostname, GOPHER_MAX_HOSTNAME_LEN - 1);
                client->items[count].hostname[GOPHER_MAX_HOSTNAME_LEN - 1] = '\0';
                client->items[count].port = client->port;
                
                /* Increment count and move to next line */
                count++;
                if (line_end + 2 <= buffer_end) {
                    line_start = line_end + 2;
                } else {
                    /* We've reached the end */
                    break;
                }
                continue;
            } else {
                /* Invalid format for non-info items, skip this line */
                if (line_end + 2 <= buffer_end) {
                    line_start = line_end + 2;
                } else {
                    /* We've reached the end */
                    break;
                }
                continue;
            }
        }
        
        /* Copy display string */
        size_t len = field_end - field_start;
        if (len >= GOPHER_MAX_SELECTOR_LEN) {
            len = GOPHER_MAX_SELECTOR_LEN - 1;
        }
        
        /* Safer strncpy instead of memcpy */
        memset(client->items[count].display_string, 0, GOPHER_MAX_SELECTOR_LEN);
        if (len > 0) {
            strncpy(client->items[count].display_string, field_start, len);
        }
        client->items[count].display_string[len] = '\0';
        
        /* Move to selector field */
        field_start = field_end + 1;
        if (field_start >= buffer_end || field_start >= line_end) {
            /* Premature end of line, skip to next */
            if (line_end + 2 <= buffer_end) {
                line_start = line_end + 2;
            } else {
                /* We've reached the end */
                break;
            }
            continue;
        }
        
        /* Get selector (second field) */
        field_end = memchr(field_start, '\t', line_end - field_start);
        if (!field_end || field_end > line_end) {
            /* Invalid format, skip this line */
            if (line_end + 2 <= buffer_end) {
                line_start = line_end + 2;
            } else {
                /* We've reached the end */
                break;
            }
            continue;
        }
        
        /* Copy selector */
        len = field_end - field_start;
        if (len >= GOPHER_MAX_SELECTOR_LEN) {
            len = GOPHER_MAX_SELECTOR_LEN - 1;
        }
        
        /* Safer strncpy instead of memcpy */
        memset(client->items[count].selector, 0, GOPHER_MAX_SELECTOR_LEN);
        if (len > 0) {
            strncpy(client->items[count].selector, field_start, len);
        }
        client->items[count].selector[len] = '\0';
        
        /* Move to hostname field */
        field_start = field_end + 1;
        if (field_start >= buffer_end || field_start >= line_end) {
            /* Premature end of line, skip to next */
            if (line_end + 2 <= buffer_end) {
                line_start = line_end + 2;
            } else {
                /* We've reached the end */
                break;
            }
            continue;
        }
        
        /* Get hostname (third field) */
        field_end = memchr(field_start, '\t', line_end - field_start);
        if (!field_end || field_end > line_end) {
            /* Invalid format, skip this line */
            if (line_end + 2 <= buffer_end) {
                line_start = line_end + 2;
            } else {
                /* We've reached the end */
                break;
            }
            continue;
        }
        
        /* Copy hostname */
        len = field_end - field_start;
        if (len >= GOPHER_MAX_HOSTNAME_LEN) {
            len = GOPHER_MAX_HOSTNAME_LEN - 1;
        }
        
        /* Safer strncpy instead of memcpy */
        memset(client->items[count].hostname, 0, GOPHER_MAX_HOSTNAME_LEN);
        if (len > 0) {
            strncpy(client->items[count].hostname, field_start, len);
        }
        client->items[count].hostname[len] = '\0';
        
        /* Move to port field */
        field_start = field_end + 1;
        if (field_start >= buffer_end || field_start >= line_end) {
            /* Premature end of line, skip to next */
            if (line_end + 2 <= buffer_end) {
                line_start = line_end + 2;
            } else {
                /* We've reached the end */
                break;
            }
            continue;
        }
        
        /* Get port (fourth field) - safely convert to integer */
        /* First create a null-terminated temporary buffer for the port string */
        char port_str[16] = {0};
        len = MIN(line_end - field_start, sizeof(port_str) - 1);
        
        /* Extract port string with bounds checking */
        if (len > 0) {
            strncpy(port_str, field_start, len);
            port_str[len] = '\0';
            
            /* Convert to integer with error handling */
            long port_val = strtol(port_str, NULL, 10);
            if (port_val <= 0 || port_val > UINT16_MAX) {
                /* Invalid port, use default */
                client->items[count].port = GOPHER_DEFAULT_PORT;
            } else {
                client->items[count].port = (uint16_t)port_val;
            }
        } else {
            /* No port specified, use default */
            client->items[count].port = GOPHER_DEFAULT_PORT;
        }
        
        /* Check for Gopher+ attributes - we'll ignore them for now */
        
        /* Increment count and move to next line */
        count++;
        if (line_end + 2 <= buffer_end) {
            line_start = line_end + 2;
        } else {
            /* We've reached the end */
            break;
        }
    }
    
    client->item_count = count;
    /* Directory parsing complete */
    return count;
}

/* Get string representation of item type */
const char *gopher_type_to_str(char type)
{
    switch (type) {
        case GOPHER_TYPE_TEXT:
            return "Text File";
        case GOPHER_TYPE_DIRECTORY:
            return "Directory";
        case GOPHER_TYPE_CSO:
            return "CSO Phone-book Server";
        case GOPHER_TYPE_ERROR:
            return "Error";
        case GOPHER_TYPE_BINHEX:
            return "BinHexed Macintosh File";
        case GOPHER_TYPE_DOS:
            return "DOS Binary";
        case GOPHER_TYPE_UUENCODED:
            return "UNIX uuencoded File";
        case GOPHER_TYPE_SEARCH:
            return "Search Server";
        case GOPHER_TYPE_TELNET:
            return "Telnet Session";
        case GOPHER_TYPE_BINARY:
            return "Binary File";
        case GOPHER_TYPE_REDUNDANT:
            return "Redundant Server";
        case GOPHER_TYPE_TN3270:
            return "TN3270 Session";
        case GOPHER_TYPE_GIF:
            return "GIF Image (g)";
        case GOPHER_TYPE_IMAGE:
            return "Image (I)";
        case 'i':
            return "Info Line";
        default:
            return "Unknown";
    }
}
