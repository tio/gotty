/*
 * tio - a simple serial terminal I/O tool
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

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <termios.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>
#include "config.h"
#include "configfile.h"
#include "tty.h"
#include "print.h"
#include "options.h"
#include "misc.h"
#include "log.h"
#include "error.h"
#include "socket.h"
#include "setspeed.h"
#include "rs485.h"
#include "alert.h"
#include "timestamp.h"
#include "misc.h"

#if defined(__APPLE__)
#define PATH_SERIAL_DEVICES "/dev/"
#define PREFIX_TTY_DEVICES "tty."
#elif defined(__CYGWIN__)
#define PATH_SERIAL_DEVICES "/dev/"
#define PREFIX_TTY_DEVICES "ttyS"
#else
#define PATH_SERIAL_DEVICES "/dev/serial/by-id/"
#define PREFIX_TTY_DEVICES ""
#endif

#ifndef CMSPAR
#define CMSPAR   010000000000
#endif

#define KEY_0 0x30
#define KEY_1 0x31
#define KEY_2 0x32
#define KEY_3 0x33
#define KEY_4 0x34
#define KEY_5 0x35
#define KEY_QUESTION 0x3f
#define KEY_B 0x62
#define KEY_C 0x63
#define KEY_E 0x65
#define KEY_F 0x66
#define KEY_SHIFT_F 0x46
#define KEY_G 0x67
#define KEY_H 0x68
#define KEY_L 0x6C
#define KEY_SHIFT_L 0x4C
#define KEY_M 0x6D
#define KEY_P 0x70
#define KEY_Q 0x71
#define KEY_S 0x73
#define KEY_T 0x74
#define KEY_U 0x55
#define KEY_V 0x76
#define KEY_Z 0x7a

enum line_mode_t
{
    LINE_OFF,
    LINE_TOGGLE,
    LINE_PULSE
};

const char random_array[] =
{
0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x28, 0x20, 0x28, 0x0A, 0x20,
0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x29, 0x20, 0x29, 0x0A, 0x20,
0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x2E, 0x2E, 0x2E, 0x2E, 0x2E, 0x2E, 0x2E,
0x2E, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x7C, 0x20, 0x20, 0x20,
0x20, 0x20, 0x20, 0x7C, 0x5D, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
0x5C, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x2F, 0x0A, 0x20, 0x20, 0x20, 0x20,
0x20, 0x20, 0x20, 0x20, 0x60, 0x2D, 0x2D, 0x2D, 0x2D, 0x27, 0x0A, 0x0A, 0x54,
0x69, 0x6D, 0x65, 0x20, 0x66, 0x6F, 0x72, 0x20, 0x61, 0x20, 0x63, 0x6F, 0x66,
0x66, 0x65, 0x65, 0x20, 0x62, 0x72, 0x65, 0x61, 0x6B, 0x21, 0x0A, 0x20, 0x0A,
0x00
};

bool interactive_mode = true;

static struct termios tio, tio_old, stdout_new, stdout_old, stdin_new, stdin_old;
static unsigned long rx_total = 0, tx_total = 0;
static bool connected = false;
static bool standard_baudrate = true;
static void (*print)(char c);
static int fd;
static bool map_i_nl_crnl = false;
static bool map_o_cr_nl = false;
static bool map_o_nl_crnl = false;
static bool map_o_del_bs = false;
static bool map_o_ltu = false;
static bool map_o_msblsb = false;
static char hex_chars[2];
static unsigned char hex_char_index = 0;
static char tty_buffer[BUFSIZ*2];
static size_t tty_buffer_count = 0;
static char *tty_buffer_write_ptr = tty_buffer;
static pthread_t thread;
static int pipefd[2];
static pthread_mutex_t mutex_input_ready = PTHREAD_MUTEX_INITIALIZER;

static void optional_local_echo(char c)
{
    if (!option.local_echo)
    {
        return;
    }
    print(c);
    if (option.log)
    {
        log_putc(c);
    }
}

inline static bool is_valid_hex(char c)
{
    return ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
}

inline static unsigned char char_to_nibble(char c)
{
    if(c >= '0' && c <= '9')
    {
        return c - '0';
    }
    else if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    else if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    else
    {
        return 0;
    }
}

void tty_sync(int fd)
{
    ssize_t count;

    while (tty_buffer_count > 0)
    {
        count = write(fd, tty_buffer, tty_buffer_count);
        if (count < 0)
        {
            // Error
            tio_debug_printf("Write error while flushing tty buffer (%s)", strerror(errno));
            break;
        }
        tty_buffer_count -= count;
        fsync(fd);
        tcdrain(fd);
    }

    // Reset
    tty_buffer_write_ptr = tty_buffer;
    tty_buffer_count = 0;
}

ssize_t tty_write(int fd, const void *buffer, size_t count)
{
    ssize_t retval = 0, bytes_written = 0;
    size_t i;

    if (map_o_ltu)
    {
        // Convert lower case to upper case
        for (i = 0; i<count; i++)
        {
            *((unsigned char*)buffer+i) = toupper(*((unsigned char*)buffer+i));
        }
    }

    if (option.output_delay || option.output_line_delay)
    {
        // Write byte by byte with output delay
        for (i=0; i<count; i++)
        {
            retval = write(fd, buffer, 1);
            if (retval < 0)
            {
                // Error
                tio_debug_printf("Write error (%s)", strerror(errno));
                break;
            }
            bytes_written += retval;

            if (option.output_line_delay && *(unsigned char*)buffer == '\n')
            {
                delay(option.output_line_delay);
            }

            fsync(fd);
            tcdrain(fd);

            if (option.output_delay)
            {
                delay(option.output_delay);
            }
        }
    }
    else
    {
        // Force write of tty buffer if too full
        if ((tty_buffer_count + count) > BUFSIZ)
        {
            tty_sync(fd);
        }

        // Copy bytes to tty write buffer
        memcpy(tty_buffer_write_ptr, buffer, count);
        tty_buffer_write_ptr += count;
        tty_buffer_count += count;
        bytes_written = count;
    }

    return bytes_written;
}

void *tty_stdin_input_thread(void *arg)
{
    UNUSED(arg);
    char input_buffer[BUFSIZ];
    ssize_t byte_count;
    ssize_t bytes_written;

    // Create FIFO pipe
    if (pipe(pipefd) == -1)
    {
        tio_error_printf("Failed to create pipe");
        exit(EXIT_FAILURE);
    }

    // Signal that input pipe is ready
    pthread_mutex_unlock(&mutex_input_ready);

    // Input loop for stdin
    while (1)
    {
        /* Input from stdin ready */
        byte_count = read(STDIN_FILENO, input_buffer, BUFSIZ);
        if (byte_count < 0)
        {
            tio_warning_printf("Could not read from stdin (%s)", strerror(errno));
        }
        else if (byte_count == 0)
        {
            // Close write end to signal EOF in read end
            close(pipefd[1]);
            pthread_exit(0);
        }

        if (interactive_mode)
        {
            static char previous_char = 0;
            char input_char;

            // Process quit and flush key command
            for (int i = 0; i<byte_count; i++)
            {
                input_char = input_buffer[i];

                if (previous_char == option.prefix_code)
                {
                    if (input_char == option.prefix_code)
                    {
                        previous_char = 0;
                        continue;
                    }

                    switch (input_char)
                    {
                        case KEY_Q:
                            exit(EXIT_SUCCESS);
                            break;
                        case KEY_SHIFT_F:
                            tio_printf("Flushed data I/O channels")
                            tcflush(fd, TCIOFLUSH);
                            break;
                        default:
                            break;
                    }
                }
                previous_char = input_char;
            }
        }

        // Write all bytes read to pipe
        while (byte_count)
        {
            bytes_written = write(pipefd[1], input_buffer, byte_count);
            if (bytes_written < 0)
            {
                tio_warning_printf("Could not write to pipe (%s)", strerror(errno));
                break;
            }
            byte_count -= bytes_written;
        }
    }

    pthread_exit(0);
}

void tty_input_thread_create(void)
{
    pthread_mutex_lock(&mutex_input_ready);

    if (pthread_create(&thread, NULL, tty_stdin_input_thread, NULL) != 0) {
        tio_error_printf("pthread_create() error");
        exit(1);
    }
}

void tty_input_thread_wait_ready(void)
{
    pthread_mutex_lock(&mutex_input_ready);
}

static void output_hex(char c)
{
    hex_chars[hex_char_index++] = c;

    printf("%c", c);

    if (hex_char_index == 2)
    {
        usleep(100*1000);
        printf("\b \b");
        printf("\b \b");

        unsigned char hex_value = char_to_nibble(hex_chars[0]) << 4 | (char_to_nibble(hex_chars[1]) & 0x0F);
        hex_char_index = 0;

        optional_local_echo(hex_value);

        ssize_t status = tty_write(fd, &hex_value, 1);
        if (status < 0)
        {
            tio_warning_printf("Could not write to tty device");
        }
        else
        {
            tx_total++;
        }
    }
}

static void toggle_line(const char *line_name, int mask, enum line_mode_t line_mode)
{
    int state;

    if (line_mode == LINE_TOGGLE)
    {
        // Toggle line
        if (ioctl(fd, TIOCMGET, &state) < 0)
        {
            tio_warning_printf("Could not get line state (%s)", strerror(errno));
        }
        else
        {
            if (state & mask)
            {
                state &= ~mask;
                tio_printf("Setting %s to LOW", line_name);
            }
            else
            {
                state |= mask;
                tio_printf("Setting %s to HIGH", line_name);
            }
            if (ioctl(fd, TIOCMSET, &state) < 0)
                tio_warning_printf("Could not set line state (%s)", strerror(errno));
        }
    } else if (line_mode == LINE_PULSE)
    {
        int duration = 0;
        // Pulse line
        toggle_line(line_name, mask, LINE_TOGGLE);
        switch (mask)
        {
            case TIOCM_DTR:
                duration = option.dtr_pulse_duration;
                break;
            case TIOCM_RTS:
                duration = option.rts_pulse_duration;
                break;
            case TIOCM_CTS:
                duration = option.cts_pulse_duration;
                break;
            case TIOCM_DSR:
                duration = option.dsr_pulse_duration;
                break;
            case TIOCM_CD:
                duration = option.dcd_pulse_duration;
                break;
            case TIOCM_RI:
                duration = option.ri_pulse_duration;
                break;
            default:
                duration = 0;
                break;
        }
        if (duration > 0)
        {
            tio_printf("Waiting %d ms", duration);
            delay(duration);
        }
        toggle_line(line_name, mask, LINE_TOGGLE);
    }
}

void handle_command_sequence(char input_char, char *output_char, bool *forward)
{
    char unused_char;
    bool unused_bool;
    int state;
    static enum line_mode_t line_mode = LINE_OFF;
    static char previous_char = 0;

    /* Ignore unused arguments */
    if (output_char == NULL)
    {
        output_char = &unused_char;
    }

    if (forward == NULL)
    {
        forward = &unused_bool;
    }

    if (line_mode)
    {
        // Handle line toggle number action
        *forward = false;
        switch (input_char)
        {
            case KEY_0:
                toggle_line("DTR", TIOCM_DTR, line_mode);
                break;
            case KEY_1:
                toggle_line("RTS", TIOCM_RTS, line_mode);
                break;
            case KEY_2:
                toggle_line("CTS", TIOCM_CTS, line_mode);
                break;
            case KEY_3:
                toggle_line("DSR", TIOCM_DSR, line_mode);
                break;
            case KEY_4:
                toggle_line("DCD", TIOCM_CD, line_mode);
                break;
            case KEY_5:
                toggle_line("RI", TIOCM_RI, line_mode);
                break;
            default:
                tio_warning_printf("Invalid line number");
                break;
        }

        line_mode = LINE_OFF;

        return;
    }

    /* Handle escape key commands */
    if (previous_char == option.prefix_code)
    {
        /* Do not forward input char to output by default */
        *forward = false;

        /* Handle special double prefix key input case */
        if (input_char == option.prefix_code)
        {
            /* Forward prefix character to tty */
            *output_char = option.prefix_code;
            *forward = true;
            previous_char = 0;
            return;
        }

        switch (input_char)
        {
            case KEY_QUESTION:
                tio_printf("Key commands:");
                tio_printf(" ctrl-%c ?       List available key commands", option.prefix_key);
                tio_printf(" ctrl-%c b       Send break", option.prefix_key);
                tio_printf(" ctrl-%c c       Show configuration", option.prefix_key);
                tio_printf(" ctrl-%c e       Toggle local echo mode", option.prefix_key);
                tio_printf(" ctrl-%c f       Toggle log to file", option.prefix_key);
                tio_printf(" ctrl-%c F       Flush data I/O buffers", option.prefix_key);
                tio_printf(" ctrl-%c g       Toggle serial port line", option.prefix_key);
                tio_printf(" ctrl-%c h       Toggle hexadecimal mode", option.prefix_key);
                tio_printf(" ctrl-%c l       Clear screen", option.prefix_key);
                tio_printf(" ctrl-%c L       Show line states", option.prefix_key);
                tio_printf(" ctrl-%c m       Toggle MSB to LSB bit order", option.prefix_key);
                tio_printf(" ctrl-%c p       Pulse serial port line", option.prefix_key);
                tio_printf(" ctrl-%c q       Quit", option.prefix_key);
                tio_printf(" ctrl-%c s       Show statistics", option.prefix_key);
                tio_printf(" ctrl-%c t       Toggle line timestamp mode", option.prefix_key);
                tio_printf(" ctrl-%c U       Toggle conversion to uppercase on output", option.prefix_key);
                tio_printf(" ctrl-%c v       Show version", option.prefix_key);
                tio_printf(" ctrl-%c ctrl-%c  Send ctrl-%c character", option.prefix_key, option.prefix_key, option.prefix_key);
                break;

            case KEY_SHIFT_L:
                if (ioctl(fd, TIOCMGET, &state) < 0)
                {
                    tio_warning_printf("Could not get line state (%s)", strerror(errno));
                    break;
                }
                tio_printf("Line states:");
                tio_printf(" DTR: %s", (state & TIOCM_DTR) ? "HIGH" : "LOW");
                tio_printf(" RTS: %s", (state & TIOCM_RTS) ? "HIGH" : "LOW");
                tio_printf(" CTS: %s", (state & TIOCM_CTS) ? "HIGH" : "LOW");
                tio_printf(" DSR: %s", (state & TIOCM_DSR) ? "HIGH" : "LOW");
                tio_printf(" DCD: %s", (state & TIOCM_CD) ? "HIGH" : "LOW");
                tio_printf(" RI : %s", (state & TIOCM_RI) ? "HIGH" : "LOW");
                break;

            case KEY_F:
                if (option.log)
                {
                    log_close();
                    option.log = false;
                }
                else
                {
                    if (log_open(option.log_filename) == 0)
                    {
                        option.log = true;
                    }
                }
                tio_printf("Switched log to file %s", option.log ? "on" : "off");
                break;

            case KEY_SHIFT_F:
                break;

            case KEY_G:
                tio_printf("Please enter which serial line number to toggle:");
                tio_printf(" DTR (0)");
                tio_printf(" RTS (1)");
                tio_printf(" CTS (2)");
                tio_printf(" DSR (3)");
                tio_printf(" DCD (4)");
                tio_printf(" RI  (5)");
                // Process next input character as part of the line toggle step
                line_mode = LINE_TOGGLE;
                break;

            case KEY_P:
                tio_printf("Please enter which serial line number to pulse:");
                tio_printf(" DTR (0)");
                tio_printf(" RTS (1)");
                tio_printf(" CTS (2)");
                tio_printf(" DSR (3)");
                tio_printf(" DCD (4)");
                tio_printf(" RI  (5)");
                // Process next input character as part of the line pulse step
                line_mode = LINE_PULSE;
                break;

            case KEY_B:
                tcsendbreak(fd, 0);
                break;

            case KEY_C:
                tio_printf("Configuration:");
                options_print();
                config_file_print();
                if (option.rs485)
                {
                    rs485_print_config();
                }
                break;

            case KEY_E:
                option.local_echo = !option.local_echo;
                tio_printf("Switched local echo %s", option.local_echo ? "on" : "off");
                break;

            case KEY_H:
                /* Toggle hexadecimal printing mode */
                if (!option.hex_mode)
                {
                    print = print_hex;
                    option.hex_mode = true;
                    tio_printf("Switched to hexadecimal mode");
                }
                else
                {
                    print = print_normal;
                    option.hex_mode = false;
                    tio_printf("Switched to normal mode");
                }
                break;

            case KEY_L:
                /* Clear screen using ANSI/VT100 escape code */
                printf("\033c");
                break;

            case KEY_M:
                /* Toggle bit order */
                if (!map_o_msblsb)
                {
                    map_o_msblsb = true;
                    tio_printf("Switched to reverse bit order");
                }
                else
                {
                    map_o_msblsb = false;
                    tio_printf("Switched to normal bit order");
                }
                break;

            case KEY_Q:
                /* Exit upon ctrl-t q sequence */
                exit(EXIT_SUCCESS);

            case KEY_S:
                /* Show tx/rx statistics upon ctrl-t s sequence */
                tio_printf("Statistics:");
                tio_printf(" Sent %lu bytes", tx_total);
                tio_printf(" Received %lu bytes", rx_total);
                break;

            case KEY_T:
                option.timestamp += 1;
                switch (option.timestamp)
                {
                    case TIMESTAMP_NONE:
                        break;
                    case TIMESTAMP_24HOUR:
                        tio_printf("Switched to 24hour timestamp mode");
                        break;
                    case TIMESTAMP_24HOUR_START:
                        tio_printf("Switched to 24hour-start timestamp mode");
                        break;
                    case TIMESTAMP_24HOUR_DELTA:
                        tio_printf("Switched to 24hour-delta timestamp mode");
                        break;
                    case TIMESTAMP_ISO8601:
                        tio_printf("Switched to iso8601 timestamp mode");
                        break;
                    case TIMESTAMP_END:
                        option.timestamp = TIMESTAMP_NONE;
                        tio_printf("Switched timestamp off");
                        break;
                }
                break;

            case KEY_U:
                map_o_ltu = !map_o_ltu;
                break;

            case KEY_V:
                tio_printf("tio v%s", VERSION);
                break;

            case KEY_Z:
                tio_printf_array(random_array);
                break;

            default:
                /* Ignore unknown ctrl-t escaped keys */
                break;
        }
    }

    previous_char = input_char;
}

void stdin_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &stdin_old);
}

void stdin_configure(void)
{
    int status;

    /* Save current stdin settings */
    if (tcgetattr(STDIN_FILENO, &stdin_old) < 0)
    {
        tio_error_printf("Saving current stdin settings failed");
        exit(EXIT_FAILURE);
    }

    /* Prepare new stdin settings */
    memcpy(&stdin_new, &stdin_old, sizeof(stdin_old));

    /* Reconfigure stdin (RAW configuration) */
    cfmakeraw(&stdin_new);

    /* Control characters */
    stdin_new.c_cc[VTIME] = 0; /* Inter-character timer unused */
    stdin_new.c_cc[VMIN]  = 1; /* Blocking read until 1 character received */

    /* Activate new stdin settings */
    status = tcsetattr(STDIN_FILENO, TCSANOW, &stdin_new);
    if (status == -1)
    {
        tio_error_printf("Could not apply new stdin settings (%s)", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Make sure we restore old stdin settings on exit */
    atexit(&stdin_restore);
}

void stdout_restore(void)
{
    tcsetattr(STDOUT_FILENO, TCSANOW, &stdout_old);
}

void stdout_configure(void)
{
    int status;

    /* Disable line buffering in stdout. This is necessary if we
     * want things like local echo to work correctly. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Save current stdout settings */
    if (tcgetattr(STDOUT_FILENO, &stdout_old) < 0)
    {
        tio_error_printf("Saving current stdio settings failed");
        exit(EXIT_FAILURE);
    }

    /* Prepare new stdout settings */
    memcpy(&stdout_new, &stdout_old, sizeof(stdout_old));

    /* Reconfigure stdout (RAW configuration) */
    cfmakeraw(&stdout_new);

    /* Allow ^C / SIGINT (to allow termination when piping to tio) */
    if (!interactive_mode)
    {
        stdout_new.c_lflag |= ISIG;
    }

    /* Control characters */
    stdout_new.c_cc[VTIME] = 0; /* Inter-character timer unused */
    stdout_new.c_cc[VMIN]  = 1; /* Blocking read until 1 character received */

    /* Activate new stdout settings */
    status = tcsetattr(STDOUT_FILENO, TCSANOW, &stdout_new);
    if (status == -1)
    {
        tio_error_printf("Could not apply new stdout settings (%s)", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* At start use normal print function */
    print = print_normal;

    /* Make sure we restore old stdout settings on exit */
    atexit(&stdout_restore);
}

void tty_configure(void)
{
    bool token_found = true;
    char *token = NULL;
    char *buffer;
    int status;
    speed_t baudrate;

    memset(&tio, 0, sizeof(tio));

    /* Set speed */
    switch (option.baudrate)
    {
        /* The macro below expands into switch cases autogenerated by meson
         * configure. Each switch case verifies and configures the baud
         * rate and is of the form:
         *
         * case $baudrate: baudrate = B$baudrate; break;
         *
         * Only switch cases for baud rates detected supported by the host
         * system are inserted.
         *
         * To see which baud rates are being probed see meson.build
         */
        BAUDRATE_CASES

        default:
#if defined (HAVE_TERMIOS2) || defined (HAVE_IOSSIOSPEED)
            standard_baudrate = false;
            break;
#else
            tio_error_printf("Invalid baud rate");
            exit(EXIT_FAILURE);
#endif
    }

    if (standard_baudrate)
    {
        // Set input speed
        status = cfsetispeed(&tio, baudrate);
        if (status == -1)
        {
            tio_error_printf("Could not configure input speed (%s)", strerror(errno));
            exit(EXIT_FAILURE);
        }

        // Set output speed
        status = cfsetospeed(&tio, baudrate);
        if (status == -1)
        {
            tio_error_printf("Could not configure output speed (%s)", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    /* Set databits */
    tio.c_cflag &= ~CSIZE;
    switch (option.databits)
    {
        case 5:
            tio.c_cflag |= CS5;
            break;
        case 6:
            tio.c_cflag |= CS6;
            break;
        case 7:
            tio.c_cflag |= CS7;
            break;
        case 8:
            tio.c_cflag |= CS8;
            break;
        default:
            tio_error_printf("Invalid data bits");
            exit(EXIT_FAILURE);
    }

    /* Set flow control */
    if (strcmp("hard", option.flow) == 0)
    {
        tio.c_cflag |= CRTSCTS;
        tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    }
    else if (strcmp("soft", option.flow) == 0)
    {
        tio.c_cflag &= ~CRTSCTS;
        tio.c_iflag |= IXON | IXOFF;
    }
    else if (strcmp("none", option.flow) == 0)
    {
        tio.c_cflag &= ~CRTSCTS;
        tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    }
    else
    {
        tio_error_printf("Invalid flow control");
        exit(EXIT_FAILURE);
    }

    /* Set stopbits */
    switch (option.stopbits)
    {
        case 1:
            tio.c_cflag &= ~CSTOPB;
            break;
        case 2:
            tio.c_cflag |= CSTOPB;
            break;
        default:
            tio_error_printf("Invalid stop bits");
            exit(EXIT_FAILURE);
    }

    /* Set parity */
    if (strcmp("odd", option.parity) == 0)
    {
        tio.c_cflag |= PARENB;
        tio.c_cflag |= PARODD;
    }
    else if (strcmp("even", option.parity) == 0)
    {
        tio.c_cflag |= PARENB;
        tio.c_cflag &= ~PARODD;
    }
    else if (strcmp("none", option.parity) == 0)
    {
        tio.c_cflag &= ~PARENB;
    }
    else if ( strcmp("mark", option.parity) == 0)
    {
        tio.c_cflag |= PARENB;
        tio.c_cflag |= PARODD;
        tio.c_cflag |= CMSPAR;
    }
    else if ( strcmp("space", option.parity) == 0)
    {
        tio.c_cflag |= PARENB;
        tio.c_cflag &= ~PARODD;
        tio.c_cflag |= CMSPAR;
    }
    else
    {
        tio_error_printf("Invalid parity");
        exit(EXIT_FAILURE);
    }

    /* Control, input, output, local modes for tty device */
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_oflag = 0;
    tio.c_lflag = 0;

    /* Control characters */
    tio.c_cc[VTIME] = 0; // Inter-character timer unused
    tio.c_cc[VMIN]  = 1; // Blocking read until 1 character received

    /* Configure any specified input or output mappings */
    buffer = strdup(option.map);
    while (token_found == true)
    {
        if (token == NULL)
        {
            token = strtok(buffer,",");
        }
        else
        {
            token = strtok(NULL, ",");
        }

        if (token != NULL)
        {
            if (strcmp(token,"INLCR") == 0)
            {
                tio.c_iflag |= INLCR;
            }
            else if (strcmp(token,"IGNCR") == 0)
            {
                tio.c_iflag |= IGNCR;
            }
            else if (strcmp(token,"ICRNL") == 0)
            {
                tio.c_iflag |= ICRNL;
            }
            else if (strcmp(token,"OCRNL") == 0)
            {
                map_o_cr_nl = true;
            }
            else if (strcmp(token,"ODELBS") == 0)
            {
                map_o_del_bs = true;
            }
            else if (strcmp(token,"INLCRNL") == 0)
            {
                map_i_nl_crnl = true;
            }
            else if (strcmp(token, "ONLCRNL") == 0)
            {
                map_o_nl_crnl = true;
            }
            else if (strcmp(token, "OLTU") == 0)
            {
                map_o_ltu = true;
            }
            else if (strcmp(token, "MSB2LSB") == 0)
            {
                map_o_msblsb = true;
            }
            else
            {
                printf("Error: Unknown mapping flag %s\n", token);
                exit(EXIT_FAILURE);
            }
        }
        else
        {
            token_found = false;
        }
    }
    free(buffer);
}

void tty_wait_for_device(void)
{
    fd_set rdfs;
    int    status;
    int    maxfd;
    struct timeval tv;
    static char input_char;
    static bool first = true;
    static int last_errno = 0;

    /* Loop until device pops up */
    while (true)
    {
        if (interactive_mode)
        {
            /* In interactive mode, while waiting for tty device, we need to
             * read from stdin to react on input key commands. */
            if (first)
            {
                /* Don't wait first time */
                tv.tv_sec = 0;
                tv.tv_usec = 1;
                first = false;
            }
            else
            {
                /* Wait up to 1 second for input */
                tv.tv_sec = 1;
                tv.tv_usec = 0;
            }

            FD_ZERO(&rdfs);
            FD_SET(pipefd[0], &rdfs);
            maxfd = MAX(pipefd[0], socket_add_fds(&rdfs, false));

            /* Block until input becomes available or timeout */
            status = select(maxfd + 1, &rdfs, NULL, NULL, &tv);
            if (status > 0)
            {
                if (FD_ISSET(pipefd[0], &rdfs))
                {
                    /* Input from stdin ready */

                    /* Read one character */
                    status = read(pipefd[0], &input_char, 1);
                    if (status <= 0)
                    {
                        tio_error_printf("Could not read from stdin");
                        exit(EXIT_FAILURE);
                    }

                    /* Handle commands */
                    handle_command_sequence(input_char, NULL, NULL);
                }
                socket_handle_input(&rdfs, NULL);
            }
            else if (status == -1)
            {
                tio_error_printf("select() failed (%s)", strerror(errno));
                exit(EXIT_FAILURE);
            }
        }

        /* Test for accessible device file */
        status = access(option.tty_device, R_OK);
        if (status == 0)
        {
            last_errno = 0;
            return;
        }
        else if (last_errno != errno)
        {
            tio_warning_printf("Could not open tty device (%s)", strerror(errno));
            tio_printf("Waiting for tty device..");
            last_errno = errno;
        }

        if (!interactive_mode)
        {
            /* In non-interactive mode we do not need to handle input key
             * commands so we simply sleep 1 second between checking for
             * presence of tty device */
            sleep(1);
        }
    }
}

void tty_disconnect(void)
{
    if (connected)
    {
        tio_printf("Disconnected");
        flock(fd, LOCK_UN);
        close(fd);
        connected = false;

        /* Fire alert action */
        alert_disconnect();
    }
}

void tty_restore(void)
{
    tcsetattr(fd, TCSANOW, &tio_old);

    if (option.rs485)
    {
        /* Restore original RS-485 mode */
        rs485_mode_restore(fd);
    }

    if (connected)
    {
        tty_disconnect();
    }
}

void forward_to_tty(int fd, char output_char)
{
    int status;

    /* Map output character */
    if ((output_char == 127) && (map_o_del_bs))
    {
        output_char = '\b';
    }
    if ((output_char == '\r') && (map_o_cr_nl))
    {
        output_char = '\n';
    }

    /* Map newline character */
    if ((output_char == '\n' || output_char == '\r') && (map_o_nl_crnl))
    {
        const char *crlf = "\r\n";

        optional_local_echo(crlf[0]);
        optional_local_echo(crlf[1]);
        status = tty_write(fd, crlf, 2);
        if (status < 0)
        {
            tio_warning_printf("Could not write to tty device");
        }

        tx_total += 2;
    }
    else
    {
        if (option.hex_mode)
        {
            output_hex(output_char);
        }
        else
        {
            /* Send output to tty device */
            optional_local_echo(output_char);
            status = tty_write(fd, &output_char, 1);
            if (status < 0)
            {
                tio_warning_printf("Could not write to tty device");
            }

            /* Update transmit statistics */
            tx_total++;
        }
    }
}

int tty_connect(void)
{
    fd_set rdfs;           /* Read file descriptor set */
    int    maxfd;          /* Maximum file descriptor used */
    char   input_char, output_char;
    char   input_buffer[BUFSIZ];
    static bool first = true;
    int    status;
    bool   next_timestamp = false;
    char*  now = NULL;
    struct timeval tv;
    struct timeval *tv_p = &tv;
    bool ignore_stdin = false;

    /* Open tty device */
    fd = open(option.tty_device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        tio_error_printf_silent("Could not open tty device (%s)", strerror(errno));
        goto error_open;
    }

    /* Make sure device is of tty type */
    if (!isatty(fd))
    {
        tio_error_printf("Not a tty device");
        exit(EXIT_FAILURE);;
    }

    /* Lock device file */
    status = flock(fd, LOCK_EX | LOCK_NB);
    if ((status == -1) && (errno == EWOULDBLOCK))
    {
        tio_error_printf("Device file is locked by another process");
        exit(EXIT_FAILURE);
    }

    /* Flush stale I/O data (if any) */
    tcflush(fd, TCIOFLUSH);

    /* Print connect status */
    tio_printf("Connected");
    connected = true;
    print_tainted = false;

    /* Fire alert action */
    alert_connect();

    if (option.timestamp)
    {
        next_timestamp = true;
    }

    /* Manage print output mode */
    if (option.hex_mode)
    {
        print = print_hex;
    }
    else
    {
        print = print_normal;
    }

    /* Save current port settings */
    if (tcgetattr(fd, &tio_old) < 0)
    {
        tio_error_printf_silent("Could not get port settings (%s)", strerror(errno));
        goto error_tcgetattr;
    }

#ifdef HAVE_IOSSIOSPEED
    if (!standard_baudrate)
    {
        /* OS X wants these fields left alone before setting arbitrary baud rate */
        tio.c_ispeed = tio_old.c_ispeed;
        tio.c_ospeed = tio_old.c_ospeed;
    }
#endif

    /* Manage RS-485 mode */
    if (option.rs485)
    {
        rs485_mode_enable(fd);
    }

    /* Make sure we restore tty settings on exit */
    if (first)
    {
        atexit(&tty_restore);
        first = false;
    }

    /* Activate new port settings */
    status = tcsetattr(fd, TCSANOW, &tio);
    if (status == -1)
    {
        tio_error_printf_silent("Could not apply port settings (%s)", strerror(errno));
        goto error_tcsetattr;
    }

    /* Set arbitrary baudrate (only works on supported platforms) */
    if (!standard_baudrate)
    {
        if (setspeed(fd, option.baudrate) != 0)
        {
            tio_error_printf_silent("Could not set baudrate speed (%s)", strerror(errno));
            goto error_setspeed;
        }
    }

    /* Input loop */
    while (true)
    {
        FD_ZERO(&rdfs);
        FD_SET(fd, &rdfs);
        if (!ignore_stdin)
        {
            FD_SET(pipefd[0], &rdfs);
        }
        maxfd = MAX(fd, pipefd[0]);
        maxfd = MAX(maxfd, socket_add_fds(&rdfs, true));

        /* Manage timeout */
        if ((option.response_wait) && (option.response_timeout != 0))
        {
            // Set response timeout
            tv_p->tv_sec = 0;
            tv_p->tv_usec = option.response_timeout * 1000;
        }
        else
        {
            // No timeout
            tv_p = NULL;
        }

        /* Block until input becomes available */
        status = select(maxfd + 1, &rdfs, NULL, NULL, tv_p);
        if (status > 0)
        {
            bool forward = false;
            if (FD_ISSET(fd, &rdfs))
            {
                /* Input from tty device ready */
                ssize_t bytes_read = read(fd, input_buffer, BUFSIZ);
                if (bytes_read <= 0)
                {
                    /* Error reading - device is likely unplugged */
                    tio_error_printf_silent("Could not read from tty device");
                    goto error_read;
                }

                /* Update receive statistics */
                rx_total += bytes_read;

                /* Process input byte by byte */
                for (int i=0; i<bytes_read; i++)
                {
                    input_char = input_buffer[i];

                    /* Print timestamp on new line if enabled */
                    if ((next_timestamp && input_char != '\n' && input_char != '\r') && !option.hex_mode)
                    {
                        now = timestamp_current_time();
                        if (now)
                        {
                            ansi_printf_raw("[%s] ", now);
                            if (option.log)
                            {
                                log_printf("[%s] ", now);
                            }
                            next_timestamp = false;
                        }
                    }

                    /* Convert MSB to LSB bit order */
                    if (map_o_msblsb)
                    {
                        char ch = input_char;
                        input_char = 0;
                        for (int j = 0; j < 8; ++j)
                        {
                            input_char |= ((1 << j) & ch) ? (1 << (7 - j)) : 0;
                        }
                    }

                    /* Map input character */
                    if ((input_char == '\n') && (map_i_nl_crnl) && (!map_o_msblsb))
                    {
                        print('\r');
                        print('\n');
                        if (option.timestamp)
                        {
                            next_timestamp = true;
                        }
                    }
                    else
                    {
                        /* Print received tty character to stdout */
                        print(input_char);
                    }

                    /* Write to log */
                    if (option.log)
                    {
                        log_putc(input_char);
                    }

                    socket_write(input_char);

                    print_tainted = true;

                    if (input_char == '\n' && option.timestamp)
                    {
                        next_timestamp = true;
                    }

                    if (option.response_wait)
                    {
                        if ((input_char == '\r') || (input_char == '\n'))
                        {
                             tty_sync(fd);
                             exit(EXIT_SUCCESS);
                        }
                    }
                }
            }
            else if (FD_ISSET(pipefd[0], &rdfs))
            {
                /* Input from stdin ready */
                ssize_t bytes_read = read(pipefd[0], input_buffer, BUFSIZ);
                if (bytes_read < 0)
                {
                    tio_error_printf_silent("Could not read from stdin (%s)", strerror(errno));
                    goto error_read;
                }
                else if (bytes_read == 0)
                {
                    /* Reached EOF (when piping to stdin) */
                    if (option.response_wait)
                    {
                        /* Stdin pipe closed but not blocking so stop listening
                         * to stdin in response mode.
                         *
                         * Note: select() really indicates not if data is ready
                         * but if file descriptor is non-blocking for I/O
                         * operation. */
                        ignore_stdin = true;
                    }
                    else
                    {
                        tty_sync(fd);
                        exit(EXIT_SUCCESS);
                    }
                }

                /* Process input byte by byte */
                for (int i=0; i<bytes_read; i++)
                {
                    input_char = input_buffer[i];

                    /* Forward input to output */
                    output_char = input_char;
                    forward = true;

                    if (interactive_mode)
                    {
                        /* Do not forward prefix key */
                        if (input_char == option.prefix_code)
                        {
                            forward = false;
                        }

                        /* Handle commands */
                        handle_command_sequence(input_char, &output_char, &forward);

                        if ((option.hex_mode) && (forward))
                        {
                            if (!is_valid_hex(input_char))
                            {
                                tio_warning_printf("Invalid hex character: '%d' (0x%02x)", input_char, input_char);
                                forward = false;
                            }
                        }
                    }

                    if (forward)
                    {
                        forward_to_tty(fd, output_char);
                    }
                }

                tty_sync(fd);
            }
            else
            {
                /* Input from socket ready */
                forward = socket_handle_input(&rdfs, &output_char);

                if (forward)
                {
                    forward_to_tty(fd, output_char);
                }

                tty_sync(fd);
            }
        }
        else if (status == -1)
        {
            tio_error_printf("select() failed (%s)", strerror(errno));
            exit(EXIT_FAILURE);
        }
        else
        {
            // Timeout (only happens in response wait mode)
            exit(EXIT_FAILURE);
        }
    }

    return TIO_SUCCESS;

error_setspeed:
error_tcsetattr:
error_tcgetattr:
error_read:
    tty_disconnect();
error_open:
    return TIO_ERROR;
}

void list_serial_devices(void)
{
    DIR *d = opendir(PATH_SERIAL_DEVICES);
    if (d)
    {
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL)
        {
            if ((strcmp(dir->d_name, ".")) && (strcmp(dir->d_name, "..")))
            {
                if (!strncmp(dir->d_name, PREFIX_TTY_DEVICES, sizeof(PREFIX_TTY_DEVICES) - 1))
                {
                    printf("%s%s\n", PATH_SERIAL_DEVICES, dir->d_name);
                }
            }
        }
        closedir(d);
    }
}
