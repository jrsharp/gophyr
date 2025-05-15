/*
 * Copyright (c) 2024 Gophyr
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gophyr, LOG_LEVEL_INF);

/**
 * @brief Main function for Gophyr application
 *
 * This application implements a Gopher protocol client with image rendering
 * and shell interface.
 */
int main(void)
{
    LOG_INF("Gophyr - Gopher protocol client started");
    LOG_INF("Use 'gopher help' or 'g help' for available commands");
    
    /* Shell commands are registered via SYS_INIT */
    
    return 0;
}