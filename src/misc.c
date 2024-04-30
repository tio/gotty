/*
 * tio - a serial device I/O tool
 *
 * Copyright (c) 2014-2022  Martin Lund
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#define _GNU_SOURCE  // For FNM_EXTMATCH
#include "config.h"
#include <ctype.h>
#include <dirent.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <sys/poll.h>
#include <termios.h>
#include <fnmatch.h>
#include "error.h"
#include "print.h"
#include "options.h"

void delay(long ms)
{
    struct timespec ts;

    if (ms <= 0)
    {
        return;
    }

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;

    nanosleep(&ts, NULL);
}

long string_to_long(char *string)
{
    long result;
    char *end_token;

    errno = 0;
    result = strtol(string, &end_token, 10);
    if ((errno != 0) || (*end_token != 0))
    {
        printf("Error: Invalid digit\n");
        exit(EXIT_FAILURE);
    }

    return result;
}

int ctrl_key_code(unsigned char key)
{
    if ((key >= 'a') && (key <= 'z'))
    {
        return key & ~0x60;
    }

    return -1;
}

bool regex_match(const char *string, const char *pattern)
{
    regex_t regex;
    int status;

    if (regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB) != 0)
    {
        // No match
        return false;
    }

    status = regexec(&regex, string, (size_t) 0, NULL, 0);
    regfree(&regex);

    if (status != 0)
    {
        // No match
        return false;
    }

    // Match
    return true;
}

int read_poll(int fd, void *data, size_t len, int timeout)
{
    struct pollfd fds;
    int ret = 0;

    fds.events = POLLIN;
    fds.fd = fd;

    /* Wait data available */
    ret = poll(&fds, 1, timeout);
    if (ret < 0)
    {
        tio_error_print("%s", strerror(errno));
        return ret;
    }
    else if (ret > 0)
    {
        if (fds.revents & POLLIN)
        {
            // Read ready data
            return read(fd, data, len);
        }
    }

    /* Timeout */
    return ret;
}

// Function to calculate djb2 hash of string
unsigned long djb2_hash(const unsigned char *str)
{
    unsigned long hash = 5381;
    int c;

    while ((c = *str++))
    {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }

    return hash;
}

// Function to encode a number to base62
char *base62_encode(unsigned long num)
{
    const char base62_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    char *output = (char *) malloc(5); // 4 characters + null terminator
    if (output == NULL)
    {
        tio_error_print("Memory allocation failed");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < 4; ++i)
    {
        output[i] = base62_chars[num % 62];
        num /= 62;
    }
    output[4] = '\0';

    return output;
}

// Function to return current time
double get_current_time(void)
{
    struct timespec current_time_ts;

    if (clock_gettime(CLOCK_REALTIME, &current_time_ts) == -1)
    {
        // Error
        return -1;
    }

    return current_time_ts.tv_sec + current_time_ts.tv_nsec / 1e9;
}

bool match_patterns(const char *string, const char *patterns)
{
    char *pattern;
    char *patterns_copy;

    if ((string == NULL) || (patterns == NULL))
    {
        return false;
    }

    patterns_copy = strdup(patterns);

    // Tokenize the patterns string using strtok
    pattern = strtok(patterns_copy, ",");
    while (pattern != NULL)
    {
        // Check if the string matches the current pattern
        #ifdef FNM_EXTMATCH
            if (fnmatch(pattern, string, FNM_EXTMATCH) == 0)
        #else
            if (fnmatch(pattern, string, 0) == 0)
        #endif
        {
            free(patterns_copy);
            return true;
        }

        // Move to the next pattern
        pattern = strtok(NULL, ",");
    }

    free(patterns_copy);
    return false;
}
