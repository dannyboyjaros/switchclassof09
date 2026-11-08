/*
 * Minimal tracing for the Nintendo Switch build.
 *
 * The console has no visible stdout under the homebrew menu, so trace lines go
 * to sdmc:/switch/class09/c09trace.txt (and to stderr for nxlink).
 */
#include "ps5_trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static FILE *g_log;
static int g_init;

void c09_trace_init(void) {
    if (g_init) {
        return;
    }
    g_init = 1;
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/class09", 0777);
    g_log = fopen("sdmc:/switch/class09/c09trace.txt", "ab");
    if (g_log) {
        setvbuf(g_log, NULL, _IOLBF, 0);
    }
}

void c09_trace(const char *fmt, ...) {
    char buffer[512];
    va_list args;
    int written;

    if (!g_init) {
        c09_trace_init();
    }
    va_start(args, fmt);
    written = vsnprintf(buffer, sizeof(buffer) - 2, fmt, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    if ((size_t)written > sizeof(buffer) - 2) {
        written = (int)(sizeof(buffer) - 2);
    }
    buffer[written++] = '\n';
    buffer[written] = 0;
    fputs(buffer, stderr);
    if (g_log) {
        fputs(buffer, g_log);
        fflush(g_log);
    }
}

/* Write a 24-bit BMP from 0xAARRGGBB pixels (bottom-up rows). */
void c09_write_bmp(const char *path, const void *pixels, int width, int height) {
    unsigned char header[54];
    unsigned char *row;
    const unsigned char *src;
    FILE *file;
    int row_bytes;
    int y;

    if (!path || !pixels || width <= 0 || height <= 0) {
        return;
    }
    row_bytes = width * 3;
    row = (unsigned char *)malloc((size_t)row_bytes);
    if (!row) {
        return;
    }
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    {
        unsigned int size = 54u + (unsigned int)(row_bytes * height);
        header[2] = (unsigned char)(size);
        header[3] = (unsigned char)(size >> 8);
        header[4] = (unsigned char)(size >> 16);
        header[5] = (unsigned char)(size >> 24);
    }
    header[10] = 54;
    header[14] = 40;
    header[18] = (unsigned char)(width);
    header[19] = (unsigned char)(width >> 8);
    header[20] = (unsigned char)(width >> 16);
    header[21] = (unsigned char)(width >> 24);
    header[22] = (unsigned char)(height);
    header[23] = (unsigned char)(height >> 8);
    header[24] = (unsigned char)(height >> 16);
    header[25] = (unsigned char)(height >> 24);
    header[26] = 1;
    header[28] = 24;
    file = fopen(path, "wb");
    if (!file) {
        free(row);
        return;
    }
    fwrite(header, 1, sizeof(header), file);
    for (y = height - 1; y >= 0; y--) {
        int x;
        src = (const unsigned char *)pixels + (size_t)y * (size_t)width * 4u;
        for (x = 0; x < width; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(row, 1, (size_t)row_bytes, file);
    }
    fclose(file);
    free(row);
}
