/* Minimal VP8/Vorbis WebM player for the PS5 runtime.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "webm.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#include <vorbis/codec.h>
#include <ogg/ogg.h>

#define EBML_ID_EBML 0x1A45DFA3ULL
#define EBML_ID_SEGMENT 0x18538067ULL
#define EBML_ID_INFO 0x1549A966ULL
#define EBML_ID_TIMECODESCALE 0x2AD7B1ULL
#define EBML_ID_DURATION 0x4489ULL
#define EBML_ID_TRACKS 0x1654AE6BULL
#define EBML_ID_TRACKENTRY 0xAEULL
#define EBML_ID_TRACKNUMBER 0xD7ULL
#define EBML_ID_TRACKTYPE 0x83ULL
#define EBML_ID_CODECID 0x86ULL
#define EBML_ID_CODECPRIVATE 0x63A2ULL
#define EBML_ID_VIDEO 0xE0ULL
#define EBML_ID_PIXELWIDTH 0xB0ULL
#define EBML_ID_PIXELHEIGHT 0xBAULL
#define EBML_ID_AUDIO 0xE1ULL
#define EBML_ID_SAMPLINGFREQUENCY 0xB5ULL
#define EBML_ID_CHANNELS 0x9FULL
#define EBML_ID_CLUSTER 0x1F43B675ULL
#define EBML_ID_TIMECODE 0xE7ULL
#define EBML_ID_SIMPLEBLOCK 0xA3ULL
#define EBML_ID_BLOCKGROUP 0xA0ULL
#define EBML_ID_BLOCK 0xA1ULL

typedef struct {
    double pts;
    const unsigned char *data;
    size_t size;
    int key;
} WebmBlock;

typedef struct {
    WebmBlock *items;
    size_t count;
    size_t capacity;
} WebmBlockList;

struct WebmPlayer {
    unsigned char *file;
    size_t file_size;

    unsigned long long timecode_scale;
    double duration;

    int video_track;
    int audio_track;
    int width;
    int height;
    int sample_rate;
    int channels;
    unsigned char *vorbis_private;
    size_t vorbis_private_size;

    WebmBlockList video;
    WebmBlockList audio;

    vpx_codec_ctx_t vpx;
    int vpx_ready;

    unsigned char *rgba;
    int frame_width;
    int frame_height;
    int frame_serial;
    int rgba_valid;
    size_t video_index;
    double last_time;
};

static int read_file(const char *path, unsigned char **data, size_t *size) {
    FILE *file;
    long length;
    unsigned char *buffer;
    file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    length = ftell(file);
    if (length <= 0) {
        fclose(file);
        return 0;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    buffer = (unsigned char *)malloc((size_t)length);
    if (!buffer) {
        fclose(file);
        return 0;
    }
    if (fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        free(buffer);
        fclose(file);
        return 0;
    }
    fclose(file);
    *data = buffer;
    *size = (size_t)length;
    return 1;
}

static int read_vint(const unsigned char *data, size_t size, size_t *pos, unsigned long long *value, int keep_marker) {
    unsigned char first;
    unsigned char mask;
    int length;
    int i;
    unsigned long long result;

    if (*pos >= size) {
        return 0;
    }
    first = data[*pos];
    mask = 0x80;
    length = 1;
    while (length <= 8 && (first & mask) == 0) {
        mask >>= 1;
        length++;
    }
    if (length > 8 || *pos + (size_t)length > size) {
        return 0;
    }
    result = keep_marker ? (unsigned long long)first : (unsigned long long)(first & (mask - 1));
    for (i = 1; i < length; i++) {
        result = (result << 8) | (unsigned long long)data[*pos + (size_t)i];
    }
    *value = result;
    *pos += (size_t)length;
    return length;
}

static unsigned long long read_uint(const unsigned char *data, size_t size) {
    unsigned long long value = 0;
    size_t i;
    for (i = 0; i < size && i < 8; i++) {
        value = (value << 8) | (unsigned long long)data[i];
    }
    return value;
}

static double read_float(const unsigned char *data, size_t size) {
    if (size == 4) {
        unsigned int bits = (unsigned int)read_uint(data, 4);
        float value;
        memcpy(&value, &bits, sizeof(value));
        return (double)value;
    }
    if (size == 8) {
        unsigned long long bits = read_uint(data, 8);
        double value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }
    return 0.0;
}

static int block_list_append(WebmBlockList *list, double pts, const unsigned char *data, size_t size, int key) {
    WebmBlock *block;
    if (list->count >= list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 128;
        WebmBlock *items = (WebmBlock *)realloc(list->items, capacity * sizeof(WebmBlock));
        if (!items) {
            return 0;
        }
        list->items = items;
        list->capacity = capacity;
    }
    block = &list->items[list->count++];
    block->pts = pts;
    block->data = data;
    block->size = size;
    block->key = key;
    return 1;
}

static void parse_info(WebmPlayer *player, size_t start, size_t end) {
    size_t pos = start;
    while (pos < end) {
        unsigned long long id;
        unsigned long long size;
        size_t data_pos;
        size_t data_end;
        if (!read_vint(player->file, end, &pos, &id, 1)) {
            break;
        }
        if (!read_vint(player->file, end, &pos, &size, 0)) {
            break;
        }
        data_pos = pos;
        data_end = data_pos + (size_t)size;
        if (data_end > end || data_end < data_pos) {
            data_end = end;
        }
        if (id == EBML_ID_TIMECODESCALE) {
            player->timecode_scale = read_uint(player->file + data_pos, data_end - data_pos);
        } else if (id == EBML_ID_DURATION) {
            double ticks = read_float(player->file + data_pos, data_end - data_pos);
            if (ticks > 0.0) {
                player->duration = ticks * (double)player->timecode_scale / 1e9;
            }
        }
        pos = data_end;
    }
}

static void parse_video_entry(WebmPlayer *player, size_t start, size_t end, int *width, int *height) {
    size_t pos = start;
    while (pos < end) {
        unsigned long long id;
        unsigned long long size;
        size_t data_pos;
        size_t data_end;
        if (!read_vint(player->file, end, &pos, &id, 1)) {
            break;
        }
        if (!read_vint(player->file, end, &pos, &size, 0)) {
            break;
        }
        data_pos = pos;
        data_end = data_pos + (size_t)size;
        if (data_end > end || data_end < data_pos) {
            data_end = end;
        }
        if (id == EBML_ID_PIXELWIDTH) {
            *width = (int)read_uint(player->file + data_pos, data_end - data_pos);
        } else if (id == EBML_ID_PIXELHEIGHT) {
            *height = (int)read_uint(player->file + data_pos, data_end - data_pos);
        }
        pos = data_end;
    }
}

static void parse_audio_entry(WebmPlayer *player, size_t start, size_t end, int *rate, int *channels) {
    size_t pos = start;
    while (pos < end) {
        unsigned long long id;
        unsigned long long size;
        size_t data_pos;
        size_t data_end;
        if (!read_vint(player->file, end, &pos, &id, 1)) {
            break;
        }
        if (!read_vint(player->file, end, &pos, &size, 0)) {
            break;
        }
        data_pos = pos;
        data_end = data_pos + (size_t)size;
        if (data_end > end || data_end < data_pos) {
            data_end = end;
        }
        if (id == EBML_ID_SAMPLINGFREQUENCY) {
            *rate = (int)(read_float(player->file + data_pos, data_end - data_pos) + 0.5);
        } else if (id == EBML_ID_CHANNELS) {
            *channels = (int)read_uint(player->file + data_pos, data_end - data_pos);
        }
        pos = data_end;
    }
}

static void parse_track_entry(WebmPlayer *player, size_t start, size_t end) {
    size_t pos = start;
    int number = 0;
    int type = 0;
    int width = 0;
    int height = 0;
    int rate = 0;
    int channels = 0;
    char codec[32];
    const unsigned char *private_data = NULL;
    size_t private_size = 0;

    codec[0] = 0;
    while (pos < end) {
        unsigned long long id;
        unsigned long long size;
        size_t data_pos;
        size_t data_end;
        if (!read_vint(player->file, end, &pos, &id, 1)) {
            break;
        }
        if (!read_vint(player->file, end, &pos, &size, 0)) {
            break;
        }
        data_pos = pos;
        data_end = data_pos + (size_t)size;
        if (data_end > end || data_end < data_pos) {
            data_end = end;
        }
        if (id == EBML_ID_TRACKNUMBER) {
            number = (int)read_uint(player->file + data_pos, data_end - data_pos);
        } else if (id == EBML_ID_TRACKTYPE) {
            type = (int)read_uint(player->file + data_pos, data_end - data_pos);
        } else if (id == EBML_ID_CODECID) {
            size_t length = data_end - data_pos;
            if (length >= sizeof(codec)) {
                length = sizeof(codec) - 1;
            }
            memcpy(codec, player->file + data_pos, length);
            codec[length] = 0;
        } else if (id == EBML_ID_CODECPRIVATE) {
            private_data = player->file + data_pos;
            private_size = data_end - data_pos;
        } else if (id == EBML_ID_VIDEO) {
            parse_video_entry(player, data_pos, data_end, &width, &height);
        } else if (id == EBML_ID_AUDIO) {
            parse_audio_entry(player, data_pos, data_end, &rate, &channels);
        }
        pos = data_end;
    }

    if (type == 1 && strcmp(codec, "V_VP8") == 0) {
        player->video_track = number;
        player->width = width;
        player->height = height;
    } else if (type == 2 && strcmp(codec, "A_VORBIS") == 0) {
        player->audio_track = number;
        player->sample_rate = rate > 0 ? rate : 48000;
        player->channels = channels > 0 ? channels : 2;
        if (private_data && private_size) {
            player->vorbis_private = (unsigned char *)malloc(private_size);
            if (player->vorbis_private) {
                memcpy(player->vorbis_private, private_data, private_size);
                player->vorbis_private_size = private_size;
            }
        }
    }
}

static void parse_tracks(WebmPlayer *player, size_t start, size_t end) {
    size_t pos = start;
    while (pos < end) {
        unsigned long long id;
        unsigned long long size;
        size_t data_pos;
        size_t data_end;
        if (!read_vint(player->file, end, &pos, &id, 1)) {
            break;
        }
        if (!read_vint(player->file, end, &pos, &size, 0)) {
            break;
        }
        data_pos = pos;
        data_end = data_pos + (size_t)size;
        if (data_end > end || data_end < data_pos) {
            data_end = end;
        }
        if (id == EBML_ID_TRACKENTRY) {
            parse_track_entry(player, data_pos, data_end);
        }
        pos = data_end;
    }
}

static void parse_block(WebmPlayer *player, size_t start, size_t end, unsigned long long cluster_time) {
    size_t pos = start;
    unsigned long long track;
    int relative;
    unsigned char flags;
    int lacing;
    double pts;
    size_t payload;

    if (!read_vint(player->file, end, &pos, &track, 0)) {
        return;
    }
    if (pos + 3 > end) {
        return;
    }
    relative = (int)((short)(((unsigned int)player->file[pos] << 8) | (unsigned int)player->file[pos + 1]));
    pos += 2;
    flags = player->file[pos++];
    pts = ((double)((long long)cluster_time + relative)) * (double)player->timecode_scale / 1e9;
    lacing = (flags >> 1) & 3;
    payload = pos;

    if (lacing == 0) {
        if (track == (unsigned long long)player->video_track) {
            block_list_append(&player->video, pts, player->file + payload, end - payload, (flags & 0x80) != 0);
        } else if (track == (unsigned long long)player->audio_track) {
            block_list_append(&player->audio, pts, player->file + payload, end - payload, 0);
        }
        return;
    }

    if (lacing == 1) {
        /* Xiph lacing: frame count, then (count-1) lengths. */
        int count;
        int i;
        size_t remaining;
        size_t *lengths;
        if (payload >= end) {
            return;
        }
        count = player->file[payload++] + 1;
        lengths = (size_t *)calloc((size_t)count, sizeof(size_t));
        if (!lengths) {
            return;
        }
        for (i = 0; i < count - 1; i++) {
            size_t value = 0;
            unsigned char byte;
            do {
                if (payload >= end) {
                    free(lengths);
                    return;
                }
                byte = player->file[payload++];
                value += byte;
            } while (byte == 255);
            lengths[i] = value;
        }
        remaining = end - payload;
        for (i = 0; i < count - 1; i++) {
            if (lengths[i] > remaining) {
                lengths[i] = remaining;
            }
            remaining -= lengths[i];
        }
        lengths[count - 1] = remaining;
        for (i = 0; i < count; i++) {
            if (track == (unsigned long long)player->video_track) {
                block_list_append(&player->video, pts, player->file + payload, lengths[i], (flags & 0x80) != 0);
            } else if (track == (unsigned long long)player->audio_track) {
                block_list_append(&player->audio, pts, player->file + payload, lengths[i], 0);
            }
            payload += lengths[i];
        }
        free(lengths);
        return;
    }

    if (lacing == 2) {
        /* Fixed lacing. */
        int count;
        size_t frame_size;
        int i;
        if (payload >= end) {
            return;
        }
        count = player->file[payload++] + 1;
        frame_size = (end - payload) / (size_t)count;
        for (i = 0; i < count; i++) {
            if (track == (unsigned long long)player->video_track) {
                block_list_append(&player->video, pts, player->file + payload, frame_size, (flags & 0x80) != 0);
            } else if (track == (unsigned long long)player->audio_track) {
                block_list_append(&player->audio, pts, player->file + payload, frame_size, 0);
            }
            payload += frame_size;
        }
        return;
    }

    /* EBML lacing is rare in these files; treat the payload as one packet. */
    if (track == (unsigned long long)player->video_track) {
        block_list_append(&player->video, pts, player->file + payload, end - payload, (flags & 0x80) != 0);
    } else if (track == (unsigned long long)player->audio_track) {
        block_list_append(&player->audio, pts, player->file + payload, end - payload, 0);
    }
}

static void parse_cluster(WebmPlayer *player, size_t start, size_t end) {
    size_t pos = start;
    unsigned long long cluster_time = 0;
    while (pos < end) {
        unsigned long long id;
        unsigned long long size;
        size_t data_pos;
        size_t data_end;
        if (!read_vint(player->file, end, &pos, &id, 1)) {
            break;
        }
        if (!read_vint(player->file, end, &pos, &size, 0)) {
            break;
        }
        data_pos = pos;
        data_end = data_pos + (size_t)size;
        if (data_end > end || data_end < data_pos) {
            data_end = end;
        }
        if (id == EBML_ID_TIMECODE) {
            cluster_time = read_uint(player->file + data_pos, data_end - data_pos);
        } else if (id == EBML_ID_SIMPLEBLOCK) {
            parse_block(player, data_pos, data_end, cluster_time);
        } else if (id == EBML_ID_BLOCKGROUP) {
            size_t group = data_pos;
            while (group < data_end) {
                unsigned long long child_id;
                unsigned long long child_size;
                size_t child_pos;
                size_t child_end;
                if (!read_vint(player->file, data_end, &group, &child_id, 1)) {
                    break;
                }
                if (!read_vint(player->file, data_end, &group, &child_size, 0)) {
                    break;
                }
                child_pos = group;
                child_end = child_pos + (size_t)child_size;
                if (child_end > data_end || child_end < child_pos) {
                    child_end = data_end;
                }
                if (child_id == EBML_ID_BLOCK) {
                    parse_block(player, child_pos, child_end, cluster_time);
                }
                group = child_end;
            }
        }
        pos = data_end;
    }
}

static void parse_segment(WebmPlayer *player, size_t start, size_t end) {
    size_t pos = start;
    while (pos < end) {
        unsigned long long id;
        unsigned long long size;
        size_t data_pos;
        size_t data_end;
        if (!read_vint(player->file, end, &pos, &id, 1)) {
            break;
        }
        if (!read_vint(player->file, end, &pos, &size, 0)) {
            break;
        }
        data_pos = pos;
        data_end = data_pos + (size_t)size;
        if (data_end > end || data_end < data_pos) {
            data_end = end;
        }
        if (id == EBML_ID_INFO) {
            parse_info(player, data_pos, data_end);
        } else if (id == EBML_ID_TRACKS) {
            parse_tracks(player, data_pos, data_end);
        } else if (id == EBML_ID_CLUSTER) {
            parse_cluster(player, data_pos, data_end);
        }
        pos = data_end;
    }
}

static void parse_file(WebmPlayer *player) {
    size_t pos = 0;
    while (pos < player->file_size) {
        unsigned long long id;
        unsigned long long size;
        size_t data_pos;
        size_t data_end;
        if (!read_vint(player->file, player->file_size, &pos, &id, 1)) {
            break;
        }
        if (!read_vint(player->file, player->file_size, &pos, &size, 0)) {
            break;
        }
        data_pos = pos;
        data_end = data_pos + (size_t)size;
        if (data_end > player->file_size || data_end < data_pos) {
            data_end = player->file_size;
        }
        if (id == EBML_ID_SEGMENT) {
            parse_segment(player, data_pos, data_end);
        }
        pos = data_end;
    }
}

static void yuv_to_rgba(WebmPlayer *player, const vpx_image_t *image) {
    int src_w = (int)image->d_w;
    int src_h = (int)image->d_h;
    int width = src_w;
    int height = src_h;
    int scaled;
    int xmap[1920];
    int x;
    int y;
    const unsigned char *yp = image->planes[VPX_PLANE_Y];
    const unsigned char *up = image->planes[VPX_PLANE_U];
    const unsigned char *vp = image->planes[VPX_PLANE_V];
    int ys = image->stride[VPX_PLANE_Y];
    int us = image->stride[VPX_PLANE_U];
    int vs = image->stride[VPX_PLANE_V];

    if (src_w <= 0 || src_h <= 0) {
        return;
    }
#ifdef __SWITCH__
    /* Decode keeps the original 1080p bitstream, but convert/upload at 720p so
     * the CPU only touches the pixels the Switch can actually display. */
    if (width > 1280) {
        height = (int)(((long long)src_h * 1280) / src_w);
        width = 1280;
        if (height < 1) {
            height = 1;
        }
    }
#endif
    scaled = (width != src_w || height != src_h);
    if (scaled && width <= (int)(sizeof(xmap) / sizeof(xmap[0]))) {
        for (x = 0; x < width; x++) {
            xmap[x] = (int)(((long long)x * src_w) / width);
        }
    }
    if (player->frame_width != width || player->frame_height != height || !player->rgba) {
        unsigned char *pixels = (unsigned char *)realloc(player->rgba, (size_t)width * (size_t)height * 4u);
        if (!pixels) {
            return;
        }
        player->rgba = pixels;
        player->frame_width = width;
        player->frame_height = height;
    }

    {
        static int tables_ready = 0;
        static int ytab[256], rv[256], gu[256], gv[256], bu[256];
        int i;
        if (!tables_ready) {
            for (i = 0; i < 256; i++) {
                int c = i - 128;
                ytab[i] = 298 * (i - 16);
                rv[i] = 409 * c;
                gu[i] = -100 * c;
                gv[i] = -208 * c;
                bu[i] = 516 * c;
            }
            tables_ready = 1;
        }
        for (y = 0; y < height; y++) {
            int sy = scaled ? (int)(((long long)y * src_h) / height) : y;
            const unsigned char *row_y = yp + (size_t)sy * (size_t)ys;
            const unsigned char *row_u = up + (size_t)(sy / 2) * (size_t)us;
            const unsigned char *row_v = vp + (size_t)(sy / 2) * (size_t)vs;
            uint32_t *out = (uint32_t *)(player->rgba + (size_t)y * (size_t)width * 4u);
            if (scaled) {
                for (x = 0; x < width; x++) {
                    int sx = xmap[x];
                    int u = row_u[sx >> 1];
                    int v = row_v[sx >> 1];
                    int y0 = ytab[row_y[sx]];
                    int r = (y0 + rv[v] + 128) >> 8;
                    int g = (y0 + gu[u] + gv[v] + 128) >> 8;
                    int b = (y0 + bu[u] + 128) >> 8;
                    if ((unsigned)r > 255u) r = r < 0 ? 0 : 255;
                    if ((unsigned)g > 255u) g = g < 0 ? 0 : 255;
                    if ((unsigned)b > 255u) b = b < 0 ? 0 : 255;
                    out[x] = (255u << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
                }
                continue;
            }
            for (x = 0; x + 1 < width; x += 2) {
                int u = row_u[x >> 1];
                int v = row_v[x >> 1];
                int y0 = ytab[row_y[x]];
                int y1 = ytab[row_y[x + 1]];
                int r = (y0 + rv[v] + 128) >> 8;
                int g = (y0 + gu[u] + gv[v] + 128) >> 8;
                int b = (y0 + bu[u] + 128) >> 8;
                if ((unsigned)r > 255u) r = r < 0 ? 0 : 255;
                if ((unsigned)g > 255u) g = g < 0 ? 0 : 255;
                if ((unsigned)b > 255u) b = b < 0 ? 0 : 255;
                out[x] = (255u << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
                r = (y1 + rv[v] + 128) >> 8;
                g = (y1 + gu[u] + gv[v] + 128) >> 8;
                b = (y1 + bu[u] + 128) >> 8;
                if ((unsigned)r > 255u) r = r < 0 ? 0 : 255;
                if ((unsigned)g > 255u) g = g < 0 ? 0 : 255;
                if ((unsigned)b > 255u) b = b < 0 ? 0 : 255;
                out[x + 1] = (255u << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
            }
            if (x < width) {
                int u = row_u[x >> 1];
                int v = row_v[x >> 1];
                int y0 = ytab[row_y[x]];
                int r = (y0 + rv[v] + 128) >> 8;
                int g = (y0 + gu[u] + gv[v] + 128) >> 8;
                int b = (y0 + bu[u] + 128) >> 8;
                if ((unsigned)r > 255u) r = r < 0 ? 0 : 255;
                if ((unsigned)g > 255u) g = g < 0 ? 0 : 255;
                if ((unsigned)b > 255u) b = b < 0 ? 0 : 255;
                out[x] = (255u << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
            }
        }
    }
    player->rgba_valid = 1;
    player->frame_serial++;
}

static const vpx_image_t *decode_video_block(WebmPlayer *player, const WebmBlock *block) {
    vpx_image_t *image;
    vpx_codec_iter_t iter = NULL;
    const vpx_image_t *last = NULL;

    if (!player->vpx_ready || !block || !block->data || block->size == 0) {
        return NULL;
    }
    if (vpx_codec_decode(&player->vpx, block->data, (unsigned int)block->size, NULL, 0) != VPX_CODEC_OK) {
        return NULL;
    }
    while ((image = vpx_codec_get_frame(&player->vpx, &iter)) != NULL) {
        last = image;
    }
    return last;
}

static void rewind_video(WebmPlayer *player) {
    if (player->vpx_ready) {
        vpx_codec_destroy(&player->vpx);
    }
    memset(&player->vpx, 0, sizeof(player->vpx));
    player->vpx_ready = vpx_codec_dec_init(&player->vpx, vpx_codec_vp8_dx(), NULL, 0) == VPX_CODEC_OK;
    player->video_index = 0;
    player->last_time = 0.0;
    player->rgba_valid = 0;
    player->frame_serial++;
}

WebmPlayer *webm_open(const char *path) {
    WebmPlayer *player;
    size_t last_video = 0;
    size_t last_audio = 0;

    if (!path || !path[0]) {
        return NULL;
    }
    player = (WebmPlayer *)calloc(1, sizeof(WebmPlayer));
    if (!player) {
        return NULL;
    }
    player->timecode_scale = 1000000ULL;
    if (!read_file(path, &player->file, &player->file_size)) {
        free(player);
        return NULL;
    }
    parse_file(player);

    if (player->duration <= 0.0) {
        if (player->video.count) {
            last_video = player->video.count - 1;
        }
        if (player->audio.count) {
            last_audio = player->audio.count - 1;
        }
        if (player->video.count || player->audio.count) {
            double end = 0.0;
            if (player->video.count) {
                end = player->video.items[last_video].pts;
            }
            if (player->audio.count && player->audio.items[last_audio].pts > end) {
                end = player->audio.items[last_audio].pts;
            }
            player->duration = end + 0.1;
        }
    }

    if (player->video_track > 0) {
        rewind_video(player);
    }
    return player;
}

void webm_close(WebmPlayer *player) {
    if (!player) {
        return;
    }
    if (player->vpx_ready) {
        vpx_codec_destroy(&player->vpx);
    }
    free(player->video.items);
    free(player->audio.items);
    free(player->vorbis_private);
    free(player->rgba);
    free(player->file);
    free(player);
}

int webm_has_video(const WebmPlayer *player) { return player && player->video_track > 0; }
int webm_has_audio(const WebmPlayer *player) { return player && player->audio_track > 0; }
int webm_width(const WebmPlayer *player) { return player ? player->width : 0; }
int webm_height(const WebmPlayer *player) { return player ? player->height : 0; }
double webm_duration(const WebmPlayer *player) { return player ? player->duration : 0.0; }

int webm_current(WebmPlayer *player, const unsigned char **rgba, int *width, int *height, int *serial) {
    if (!player || !player->rgba_valid || !player->rgba) {
        return 0;
    }
    if (rgba) {
        *rgba = player->rgba;
    }
    if (width) {
        *width = player->frame_width;
    }
    if (height) {
        *height = player->frame_height;
    }
    if (serial) {
        *serial = player->frame_serial;
    }
    return 1;
}

int webm_frame(WebmPlayer *player, double t, const unsigned char **rgba, int *width, int *height, int *serial) {
    const vpx_image_t *last = NULL;
    if (!player || !player->vpx_ready) {
        return 0;
    }
    if (t + 0.001 < player->last_time) {
        rewind_video(player);
    }
    if (t < 0.0) {
        t = 0.0;
    }
    player->last_time = t;

    if (!player->rgba_valid && player->video_index < player->video.count) {
        last = decode_video_block(player, &player->video.items[player->video_index]);
        player->video_index++;
    }
    while (player->video_index < player->video.count &&
           player->video.items[player->video_index].pts <= t + 0.0005) {
        const vpx_image_t *image = decode_video_block(player, &player->video.items[player->video_index]);
        if (image) {
            last = image;
        }
        player->video_index++;
    }
    /* Convert once: intermediate frames decoded to catch up are never shown. */
    if (last) {
        yuv_to_rgba(player, last);
    }
    if (!player->rgba_valid) {
        return 0;
    }
    if (rgba) {
        *rgba = player->rgba;
    }
    if (width) {
        *width = player->frame_width;
    }
    if (height) {
        *height = player->frame_height;
    }
    if (serial) {
        *serial = player->frame_serial;
    }
    return 1;
}

static void write_u16(FILE *file, unsigned int value) {
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & 0xFF);
    bytes[1] = (unsigned char)((value >> 8) & 0xFF);
    fwrite(bytes, 1, 2, file);
}

static void write_u32(FILE *file, unsigned int value) {
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & 0xFF);
    bytes[1] = (unsigned char)((value >> 8) & 0xFF);
    bytes[2] = (unsigned char)((value >> 16) & 0xFF);
    bytes[3] = (unsigned char)((value >> 24) & 0xFF);
    fwrite(bytes, 1, 4, file);
}

static int parse_vorbis_headers(const unsigned char *data, size_t size, vorbis_info *info,
                                vorbis_comment *comment) {
    size_t pos;
    size_t lengths[2];
    int i;
    if (!data || size < 3 || data[0] < 2) {
        return 0;
    }
    pos = 1;
    for (i = 0; i < 2; i++) {
        size_t length = 0;
        unsigned char byte;
        do {
            if (pos >= size) {
                return 0;
            }
            byte = data[pos++];
            length += byte;
        } while (byte == 255);
        lengths[i] = length;
    }
    for (i = 0; i < 3; i++) {
        size_t length = (i < 2) ? lengths[i] : size - pos;
        ogg_packet packet;
        if (pos + length > size) {
            return 0;
        }
        memset(&packet, 0, sizeof(packet));
        packet.packet = (unsigned char *)(data + pos);
        packet.bytes = (long)length;
        packet.b_o_s = (i == 0) ? 1 : 0;
        packet.packetno = i;
        if (vorbis_synthesis_headerin(info, comment, &packet) != 0) {
            return 0;
        }
        pos += length;
    }
    return 1;
}

int webm_audio_to_wav(WebmPlayer *player, const char *path) {
    vorbis_info info;
    vorbis_comment comment;
    vorbis_dsp_state dsp;
    vorbis_block block;
    unsigned char *pcm_buffer = NULL;
    size_t pcm_size = 0;
    size_t pcm_capacity = 0;
    size_t i;
    int channels;
    int rate;
    int ok = 0;
    FILE *file = NULL;

    if (!player || !player->vorbis_private || !player->vorbis_private_size || !path) {
        return 0;
    }
    vorbis_info_init(&info);
    vorbis_comment_init(&comment);
    if (!parse_vorbis_headers(player->vorbis_private, player->vorbis_private_size, &info, &comment)) {
        vorbis_comment_clear(&comment);
        vorbis_info_clear(&info);
        return 0;
    }
    if (vorbis_synthesis_init(&dsp, &info) != 0) {
        vorbis_comment_clear(&comment);
        vorbis_info_clear(&info);
        return 0;
    }
    vorbis_block_init(&dsp, &block);
    channels = info.channels > 0 ? info.channels : 2;
    rate = info.rate > 0 ? info.rate : 48000;

    for (i = 0; i < player->audio.count; i++) {
        const WebmBlock *item = &player->audio.items[i];
        ogg_packet packet;
        memset(&packet, 0, sizeof(packet));
        packet.packet = (unsigned char *)item->data;
        packet.bytes = (long)item->size;
        packet.packetno = (long)i;
        if (vorbis_synthesis(&block, &packet) == 0) {
            float **samples = NULL;
            int produced;
            vorbis_synthesis_blockin(&dsp, &block);
            while ((produced = vorbis_synthesis_pcmout(&dsp, &samples)) > 0) {
                size_t needed = (size_t)produced * (size_t)channels * sizeof(short);
                if (pcm_size + needed > pcm_capacity) {
                    size_t capacity = pcm_capacity ? pcm_capacity : 65536;
                    while (capacity < pcm_size + needed) {
                        capacity *= 2;
                    }
                    {
                        unsigned char *grown = (unsigned char *)realloc(pcm_buffer, capacity);
                        if (!grown) {
                            goto cleanup;
                        }
                        pcm_buffer = grown;
                        pcm_capacity = capacity;
                    }
                }
                {
                    int frame;
                    for (frame = 0; frame < produced; frame++) {
                        int channel;
                        for (channel = 0; channel < channels; channel++) {
                            float value = samples[channel][frame];
                            int sample;
                            if (value > 1.0f) value = 1.0f;
                            if (value < -1.0f) value = -1.0f;
                            sample = (int)(value * 32767.0f);
                            pcm_buffer[pcm_size++] = (unsigned char)(sample & 0xFF);
                            pcm_buffer[pcm_size++] = (unsigned char)((sample >> 8) & 0xFF);
                        }
                    }
                }
                vorbis_synthesis_read(&dsp, produced);
            }
        }
    }

    if (pcm_size == 0) {
        goto cleanup;
    }
    file = fopen(path, "wb");
    if (!file) {
        goto cleanup;
    }
    fwrite("RIFF", 1, 4, file);
    write_u32(file, (unsigned int)(36 + pcm_size));
    fwrite("WAVEfmt ", 1, 8, file);
    write_u32(file, 16);
    write_u16(file, 1);
    write_u16(file, (unsigned int)channels);
    write_u32(file, (unsigned int)rate);
    write_u32(file, (unsigned int)(rate * channels * 2));
    write_u16(file, (unsigned int)(channels * 2));
    write_u16(file, 16);
    fwrite("data", 1, 4, file);
    write_u32(file, (unsigned int)pcm_size);
    fwrite(pcm_buffer, 1, pcm_size, file);
    fclose(file);
    file = NULL;
    ok = 1;

cleanup:
    if (file) {
        fclose(file);
    }
    free(pcm_buffer);
    vorbis_block_clear(&block);
    vorbis_dsp_clear(&dsp);
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
    return ok;
}
