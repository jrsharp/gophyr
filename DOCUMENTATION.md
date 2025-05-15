# Gophyr Documentation

## Overview

Gophyr is a Gopher protocol client for Zephyr RTOS. It provides a shell-based interface for browsing Gopher servers, viewing text files, searching, and even rendering images as ASCII art.

## Architecture

The client is composed of the following modules:

### 1. Gopher Client (`gopher_client.c/h`)

The core implementation of the Gopher protocol:
- TCP socket communication with Gopher servers
- Parsing of Gopher responses
- Management of Gopher items and their metadata
- Directory listing representation
- Response handling for different Gopher item types

### 2. Gopher Image Processor (`gopher_image.c/h`) 

Handles image processing capabilities:
- Image decoding using stb_image library
- RGB to ASCII art conversion
- Image downscaling for terminal display
- Memory management for image processing (including SPIRAM support)
- Color terminal output for enhanced visual rendering

### 3. Gopher Shell Interface (`gopher_shell.c`)

Provides the shell commands interface:
- Command registration and handling
- Network initialization
- Session management
- Navigation history implementation
- Help system

### 4. Main Application (`main.c`)

The application entry point that initializes the system and logging.

## Memory Management

The client implements specialized memory management to handle large images on memory-constrained devices:

1. **Standard Memory**: For regular operations, the system heap is used
2. **SPIRAM (ESP32 only)**: When available, SPIRAM is used for large image processing
   - The shared multi-heap API is used to allocate from SPIRAM
   - Image processing memory limit is increased from 200KB to 3MB when SPIRAM is available

## Networking

The client uses Zephyr's networking stack:
- TCP sockets for Gopher communication
- DNS resolution for hostnames
- Retries and timeout handling

## Shell Commands

### Basic Commands

- `gopher init` or `g init`: Initialize the Gopher client
- `gopher ip` or `g ip`: Display network information
- `gopher help` or `g help`: Display help information

### Connection Commands

- `gopher connect <host> [port]` or `g connect <host> [port]`: Connect to a Gopher server
- `gopher get [selector]` or `g get [selector]`: Request a document or directory

### Navigation Commands

- `gopher view <index>` or `g <index>`: View an item from the directory
- `gopher back` or `g back`: Navigate back to previous item

### Search Commands

- `gopher search <index> <query>` or `g search <index> <query>`: Search using a Gopher search service

## ASCII Art Image Rendering

The client can render images as ASCII art in the terminal:

1. Images are downloaded via the Gopher protocol
2. Decoded using the stb_image library
3. Downscaled to fit the terminal
4. Converted to ASCII characters based on brightness
5. Optionally colored using ANSI escape sequences

## SPIRAM Support for ESP32

On ESP32 targets with SPIRAM (PSRAM), the client can allocate large buffers from SPIRAM using Zephyr's shared multi-heap API. This allows processing of images up to 3MB in size.

Configuration for SPIRAM:
```
CONFIG_ESP_SPIRAM=y
CONFIG_ESP_SPIRAM_SIZE=8388608
CONFIG_ESP_SPIRAM_HEAP_SIZE=4096000
CONFIG_SHARED_MULTI_HEAP=y
```

## Error Handling

The client implements robust error handling:
- Network connectivity issues
- Memory allocation failures 
- Invalid responses
- Timeout handling
- User input validation

## Debugging

Debug output can be enabled through Zephyr's logging system:
```
CONFIG_LOG=y
CONFIG_GOPHYR_LOG_LEVEL_DBG=y
```

## Building for Different Platforms

The client has been tested on various platforms:
- ESP32 series (ESP32, ESP32S2, ESP32S3, ESP32C3)
- Nordic nRF series with Wi-Fi (nRF7002)
- NXP platforms with Wi-Fi

Board-specific configurations are provided in:
- `boards/` directory - Board configs
- `socs/` directory - SoC-specific configs
- `nxp/` directory - NXP-specific overlays