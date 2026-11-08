#include "engine.h"
#include "ps5_trace.h"

#include <math.h>

#ifdef __SWITCH__
#include <switch.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#endif

#if defined(__PROSPERO__) || defined(__SWITCH__)
#include "webm.h"
#include <unistd.h>
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#define MKDIR(path) _mkdir(path)
#define STAT _stat
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(path) mkdir(path, 0777)
#define STAT stat
#endif

#ifdef __PROSPERO__
/* The app folder is mounted read-only at /app0. Writable storage lives in
 * /download0, which the console creates because param.json has a positive
 * downloadDataSize. */
#define DEFAULT_DATA_DIR "/app0/assets"
#define DEFAULT_GAME_DIR "/app0/assets/game"
#define DEFAULT_SAVE_DIR "/download0/saves"
#elif defined(__SWITCH__)
/* Everything lives on the SD card next to the .nro. */
#define DEFAULT_DATA_DIR "sdmc:/switch/class09/assets"
#define DEFAULT_GAME_DIR "sdmc:/switch/class09/assets/game"
#define DEFAULT_SAVE_DIR "sdmc:/switch/class09/saves"
#else
#define DEFAULT_GAME_DIR "../Class of 09/game"
#endif

/* ------------------------------------------------------------------------- */
/* small utilities                                                           */
/* ------------------------------------------------------------------------- */

static void copy_str(char *dst, size_t n, const char *src) {
    size_t len;
    if (!dst || !n) {
        return;
    }
    if (!src) {
        src = "";
    }
    len = strlen(src);
    if (len >= n) {
        len = n - 1;
    }
    memcpy(dst, src, len);
    dst[len] = 0;
}

static int is_absolute(const char *path) {
    if (!path || !path[0]) {
        return 0;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return 1;
    }
    return isalpha((unsigned char)path[0]) && path[1] == ':';
}

static void join_path(char *out, size_t n, const char *base, const char *rel) {
    size_t len;
    if (!out || !n) {
        return;
    }
    if (!rel) {
        rel = "";
    }
    if (!base || !base[0] || is_absolute(rel)) {
        copy_str(out, n, rel);
        return;
    }
    copy_str(out, n, base);
    len = strlen(out);
    if (len + 1 < n && len > 0 && out[len - 1] != '/' && out[len - 1] != '\\') {
        out[len++] = '/';
        out[len] = 0;
    }
    if (len + 1 < n) {
        snprintf(out + len, n - len, "%s", rel);
    }
}

#if !defined(__PROSPERO__) && !defined(__SWITCH__)
static void dir_name(const char *path, char *out, size_t n) {
    char *slash;
    char *back;
    char *cut;
    copy_str(out, n, path ? path : "");
    slash = strrchr(out, '/');
    back = strrchr(out, '\\');
    if (slash && back) {
        cut = (slash > back) ? slash : back;
    } else {
        cut = slash ? slash : back;
    }
    if (cut) {
        *cut = 0;
    } else {
        out[0] = 0;
    }
    if (!out[0]) {
        copy_str(out, n, ".");
    }
}
#endif

static char *read_file(const char *path, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    long len;
    char *buf;
    if (!file) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    len = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (len < 0) {
        fclose(file);
        return NULL;
    }
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(file);
        return NULL;
    }
    if (len > 0 && fread(buf, 1, (size_t)len, file) != (size_t)len) {
        free(buf);
        fclose(file);
        return NULL;
    }
    buf[len] = 0;
    fclose(file);
    if (out_len) {
        *out_len = (size_t)len;
    }
    return buf;
}

static int time_reached(Uint32 deadline) {
    return (Sint32)(SDL_GetTicks() - deadline) >= 0;
}

static void set_toast(Engine *e, const char *text) {
    copy_str(e->toast, sizeof(e->toast), text);
    e->toast_until = SDL_GetTicks() + 2600;
}

/* ------------------------------------------------------------------------- */
/* JSON helpers                                                              */
/* ------------------------------------------------------------------------- */

static JsonValue *op_item(JsonValue *op, size_t index) {
    if (!op || op->type != JL_ARRAY || index >= op->u.array.count) {
        return NULL;
    }
    return op->u.array.items[index];
}

static const char *op_str(JsonValue *op, size_t index) {
    return json_as_string(op_item(op, index));
}

static int op_int(JsonValue *op, size_t index) {
    return (int)json_as_number(op_item(op, index));
}

static double op_num(JsonValue *op, size_t index) {
    return json_as_number(op_item(op, index));
}

static JsonValue *obj_get(JsonValue *object, const char *key) {
    return json_object_get(object, key);
}

static const char *obj_str(JsonValue *object, const char *key, const char *def) {
    JsonValue *value = json_object_get(object, key);
    if (value && value->type == JL_STRING) {
        return value->u.string;
    }
    return def;
}

static int obj_int(JsonValue *object, const char *key, int def) {
    JsonValue *value = json_object_get(object, key);
    if (!value) {
        return def;
    }
    return (int)json_as_number(value);
}

static double obj_num(JsonValue *object, const char *key, double def) {
    JsonValue *value = json_object_get(object, key);
    if (!value) {
        return def;
    }
    return json_as_number(value);
}

/* ATL keyframes are exported as {"value": [[time,x,y,w,h,alpha,flip], ...],
 * "Count": n}; older exports used the bare array.  Accept both. */
static JsonValue *anim_frames(JsonValue *frames) {
    JsonValue *value;
    if (!frames) {
        return NULL;
    }
    if (frames->type == JL_ARRAY) {
        return frames;
    }
    if (frames->type == JL_OBJECT) {
        value = obj_get(frames, "value");
        if (value && value->type == JL_ARRAY) {
            return value;
        }
    }
    return NULL;
}

static int label_index(Engine *e, const char *name) {
    JsonValue *value;
    if (!name || !name[0]) {
        return -1;
    }
    value = json_object_get(e->labels, name);
    if (!value) {
        return -1;
    }
    return (int)json_as_number(value);
}

/* ------------------------------------------------------------------------- */
/* colours                                                                   */
/* ------------------------------------------------------------------------- */

static Uint32 parse_color(const char *text, Uint32 def) {
    unsigned long value;
    char *end = NULL;
    if (!text || !text[0]) {
        return def;
    }
    if (text[0] == '#') {
        text++;
    }
    value = strtoul(text, &end, 16);
    if (end == text) {
        return def;
    }
    if (strlen(text) <= 3) {
        unsigned long r = (value >> 8) & 0xF;
        unsigned long g = (value >> 4) & 0xF;
        unsigned long b = value & 0xF;
        return (Uint32)((r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b);
    }
    return (Uint32)(value & 0xFFFFFF);
}

static SDL_Color color_of(Uint32 rgb) {
    SDL_Color color;
    color.r = (Uint8)((rgb >> 16) & 0xFF);
    color.g = (Uint8)((rgb >> 8) & 0xFF);
    color.b = (Uint8)(rgb & 0xFF);
    color.a = 255;
    return color;
}

/* ------------------------------------------------------------------------- */
/* texture cache                                                             */
/* ------------------------------------------------------------------------- */

static SDL_Texture *get_texture_at(Engine *e, const char *full, int *out_w, int *out_h) {
    Uint32 now;
    int i;
    int slot = -1;
    SDL_Surface *surface;
    SDL_Texture *texture;
    int w = 0;
    int h = 0;

    if (out_w) {
        *out_w = 0;
    }
    if (out_h) {
        *out_h = 0;
    }
    if (!e || !e->renderer || !full || !full[0]) {
        return NULL;
    }
    now = ++e->frame_counter;

    for (i = 0; i < e->tex_count; i++) {
        if (e->tex_cache[i].tex && strcmp(e->tex_cache[i].path, full) == 0) {
            e->tex_cache[i].refs++;
            e->tex_cache[i].last_used = now;
            if (out_w) {
                *out_w = e->tex_cache[i].w;
            }
            if (out_h) {
                *out_h = e->tex_cache[i].h;
            }
            return e->tex_cache[i].tex;
        }
    }

    surface = IMG_Load(full);
    if (!surface) {
        printf("class09: image load failed: %s (%s)\n", full, IMG_GetError());
        return NULL;
    }
    w = surface->w;
    h = surface->h;
    texture = SDL_CreateTextureFromSurface(e->renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture) {
        return NULL;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

    if (e->tex_count < TEX_CACHE) {
        slot = e->tex_count++;
    } else {
        Uint32 oldest = 0xFFFFFFFFu;
        int oldest_index = 0;
        for (i = 0; i < e->tex_count; i++) {
            if (e->tex_cache[i].refs == 0 && e->tex_cache[i].last_used < oldest) {
                oldest = e->tex_cache[i].last_used;
                oldest_index = i;
            }
        }
        if (oldest == 0xFFFFFFFFu) {
            oldest = 0xFFFFFFFFu;
            for (i = 0; i < e->tex_count; i++) {
                if (e->tex_cache[i].last_used < oldest) {
                    oldest = e->tex_cache[i].last_used;
                    oldest_index = i;
                }
            }
        }
        slot = oldest_index;
        if (e->tex_cache[slot].tex) {
            SDL_DestroyTexture(e->tex_cache[slot].tex);
        }
    }

    copy_str(e->tex_cache[slot].path, MAX_PATH, full);
    e->tex_cache[slot].tex = texture;
    e->tex_cache[slot].w = w;
    e->tex_cache[slot].h = h;
    e->tex_cache[slot].refs = 1;
    e->tex_cache[slot].last_used = now;
    if (out_w) {
        *out_w = w;
    }
    if (out_h) {
        *out_h = h;
    }
    return texture;
}

static SDL_Texture *get_texture(Engine *e, const char *rel, int *out_w, int *out_h) {
    char full[MAX_PATH];
    if (!e || !rel || !rel[0]) {
        if (out_w) {
            *out_w = 0;
        }
        if (out_h) {
            *out_h = 0;
        }
        return NULL;
    }
    join_path(full, sizeof(full), e->game_dir, rel);
    return get_texture_at(e, full, out_w, out_h);
}

static void reset_texture_refs(Engine *e) {
    int i;
    for (i = 0; i < e->tex_count; i++) {
        e->tex_cache[i].refs = 0;
    }
}

static Mix_Chunk *get_chunk(Engine *e, const char *rel) {
    char full[MAX_PATH];
    int i;
    Mix_Chunk *chunk;
    if (!e->audio_ok || !rel || !rel[0]) {
        return NULL;
    }
    join_path(full, sizeof(full), e->game_dir, rel);
    for (i = 0; i < e->chunk_count; i++) {
        if (e->chunk_cache[i].chunk && strcmp(e->chunk_cache[i].path, full) == 0) {
            return e->chunk_cache[i].chunk;
        }
    }
    if (e->chunk_count >= CHUNK_CACHE) {
        return NULL;
    }
    chunk = Mix_LoadWAV(full);
    if (!chunk) {
        return NULL;
    }
    copy_str(e->chunk_cache[e->chunk_count].path, MAX_PATH, full);
    e->chunk_cache[e->chunk_count].chunk = chunk;
    e->chunk_count++;
    return chunk;
}

/* ------------------------------------------------------------------------- */
/* fonts                                                                     */
/* ------------------------------------------------------------------------- */

static TTF_Font *open_font(Engine *e, const char *rel, int size) {
    char full[MAX_PATH];
    if (!rel || !rel[0] || size <= 0) {
        return NULL;
    }
    join_path(full, sizeof(full), e->game_dir, rel);
    return TTF_OpenFont(full, size);
}

static TTF_Font *ensure_font(Engine *e, int id) {
    static const char *fallbacks[] = {
        "HELVETICA_LT_NARROW.TTF",
        "handwriting2.ttf",
        "FUTURA.TTF",
        "/system_ex/fonts/Gontserrat-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "C:/Windows/Fonts/arial.ttf",
        NULL};
    int i;
    if (id < 0 || id >= FONT_COUNT) {
        id = FONT_SMALL;
    }
    if (e->fonts[id]) {
        return e->fonts[id];
    }
    if (e->font_sizes[id] <= 0) {
        e->font_sizes[id] = 40;
    }
    e->fonts[id] = open_font(e, e->font_paths[id], e->font_sizes[id]);
    if (!e->fonts[id]) {
        for (i = 0; fallbacks[i]; i++) {
            e->fonts[id] = open_font(e, fallbacks[i], e->font_sizes[id]);
            if (e->fonts[id]) {
                break;
            }
        }
    }
    if (!e->fonts[id]) {
        printf("class09: could not open a font for slot %d\n", id);
    }
    return e->fonts[id];
}

/* ------------------------------------------------------------------------- */
/* wrapped text cache                                                        */
/* ------------------------------------------------------------------------- */

static void push_line(TextBlock *block, const char *text) {
    size_t len;
    char *dst;
    if (!block || block->line_count >= MAX_LINES) {
        return;
    }
    dst = block->lines[block->line_count];
    len = text ? strlen(text) : 0;
    if (len > 511) {
        len = 511;
    }
    if (len) {
        memcpy(dst, text, len);
    }
    dst[len] = 0;
    while (len > 0 && (dst[len - 1] == ' ' || dst[len - 1] == '\t')) {
        dst[--len] = 0;
    }
    block->line_count++;
}

static void wrap_paragraph(TTF_Font *font, const char *text, int len, int wrap, TextBlock *block) {
    char para[512];
    char current[512];
    char candidate[1024];
    int i = 0;
    int w = 0;
    int h = 0;

    if (len < 0) {
        len = 0;
    }
    if (len > 511) {
        len = 511;
    }
    if (len) {
        memcpy(para, text, (size_t)len);
    }
    para[len] = 0;

    if (!font || wrap <= 0) {
        push_line(block, para);
        return;
    }
    TTF_SizeUTF8(font, para, &w, &h);
    if (w <= wrap) {
        push_line(block, para);
        return;
    }

    current[0] = 0;
    while (i < len) {
        int start;
        int tlen;
        char token[512];
        while (i < len && (para[i] == ' ' || para[i] == '\t')) {
            i++;
        }
        start = i;
        while (i < len && para[i] != ' ' && para[i] != '\t') {
            i++;
        }
        tlen = i - start;
        if (tlen <= 0) {
            break;
        }
        if (tlen > 511) {
            tlen = 511;
        }
        memcpy(token, para + start, (size_t)tlen);
        token[tlen] = 0;
        if (current[0]) {
            snprintf(candidate, sizeof(candidate), "%s %s", current, token);
        } else {
            snprintf(candidate, sizeof(candidate), "%s", token);
        }
        TTF_SizeUTF8(font, candidate, &w, &h);
        if (w <= wrap || !current[0]) {
            copy_str(current, sizeof(current), candidate);
        } else {
            push_line(block, current);
            copy_str(current, sizeof(current), token);
        }
    }
    if (current[0]) {
        push_line(block, current);
    }
}

static void free_text_block(TextBlock *block) {
    int i;
    for (i = 0; i < MAX_LINES; i++) {
        if (block->tex[i]) {
            SDL_DestroyTexture(block->tex[i]);
            block->tex[i] = NULL;
        }
    }
    block->used = 0;
    block->line_count = 0;
}

static TextBlock *text_block(Engine *e, int font_id, const char *text, Uint32 color, int wrap) {
    int i;
    int slot = -1;
    Uint32 now;
    Uint32 oldest;
    TextBlock *block;
    TTF_Font *font;
    const char *p;

    if (!e) {
        return NULL;
    }
    if (!text) {
        text = "";
    }
    now = ++e->frame_counter;
    for (i = 0; i < TEXT_CACHE; i++) {
        block = &e->text_cache[i];
        if (block->used && block->font == font_id && block->wrap == wrap && block->color == color &&
            strcmp(block->text, text) == 0) {
            block->last_used = now;
            return block;
        }
    }
    for (i = 0; i < TEXT_CACHE; i++) {
        if (!e->text_cache[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        oldest = 0xFFFFFFFFu;
        slot = 0;
        for (i = 0; i < TEXT_CACHE; i++) {
            if (e->text_cache[i].last_used < oldest) {
                oldest = e->text_cache[i].last_used;
                slot = i;
            }
        }
    }
    block = &e->text_cache[slot];
    free_text_block(block);
    memset(block, 0, sizeof(*block));
    block->used = 1;
    block->font = font_id;
    block->wrap = wrap;
    block->color = color;
    block->last_used = now;
    copy_str(block->text, sizeof(block->text), text);

    font = ensure_font(e, font_id);
    p = block->text;
    if (!p[0]) {
        push_line(block, "");
        return block;
    }
    while (*p && block->line_count < MAX_LINES) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        while (len > 0 && p[len - 1] == '\r') {
            len--;
        }
        wrap_paragraph(font, p, len, wrap, block);
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    if (block->line_count == 0) {
        push_line(block, "");
    }
    return block;
}

static int block_height(Engine *e, TextBlock *block, int spacing) {
    TTF_Font *font;
    int skip;
    if (!block) {
        return 0;
    }
    font = ensure_font(e, block->font);
    skip = font ? TTF_FontLineSkip(font) : 40;
    if (block->line_count <= 0) {
        return skip;
    }
    return block->line_count * skip + (block->line_count - 1) * spacing;
}

static int draw_block(Engine *e, TextBlock *block, int x, int y, int align, int spacing) {
    TTF_Font *font;
    SDL_Color color;
    int i;
    int cursor = y;
    int skip;

    if (!e || !block || !e->renderer) {
        return 0;
    }
    font = ensure_font(e, block->font);
    if (!font) {
        return 0;
    }
    color = color_of(block->color);
    skip = TTF_FontLineSkip(font);

    for (i = 0; i < block->line_count; i++) {
        const char *line = block->lines[i];
        if (line[0]) {
            if (!block->tex[i]) {
                SDL_Surface *surface = TTF_RenderUTF8_Blended(font, line, color);
                if (surface) {
                    block->tex[i] = SDL_CreateTextureFromSurface(e->renderer, surface);
                    block->tw[i] = surface->w;
                    block->th[i] = surface->h;
                    SDL_FreeSurface(surface);
                    if (block->tex[i]) {
                        SDL_SetTextureBlendMode(block->tex[i], SDL_BLENDMODE_BLEND);
                    }
                }
            }
            if (block->tex[i]) {
                SDL_Rect dst;
                dst.w = block->tw[i];
                dst.h = block->th[i];
                if (align == 1) {
                    dst.x = x - dst.w / 2;
                } else if (align == 2) {
                    dst.x = x - dst.w;
                } else {
                    dst.x = x;
                }
                dst.y = cursor;
                SDL_RenderCopy(e->renderer, block->tex[i], NULL, &dst);
            }
        }
        cursor += skip + spacing;
    }
    return cursor - y;
}

static int draw_text(Engine *e, int font_id, const char *text, Uint32 color, int wrap, int x, int y, int align,
                     int spacing) {
    TextBlock *block = text_block(e, font_id, text, color, wrap);
    return draw_block(e, block, x, y, align, spacing);
}

/* ------------------------------------------------------------------------- */
/* audio                                                                     */
/* ------------------------------------------------------------------------- */

static int channel_index(const char *name) {
    if (name && strcmp(name, "ambient") == 0) {
        return 0;
    }
    if (name && strcmp(name, "music") == 0) {
        return 1;
    }
    return 2;
}

static void apply_volumes(Engine *e) {
    if (!e->audio_ok) {
        return;
    }
    Mix_Volume(0, e->opt_music * MIX_MAX_VOLUME / 10);
    Mix_Volume(1, e->opt_music * MIX_MAX_VOLUME / 10);
    Mix_Volume(2, 8 * MIX_MAX_VOLUME / 10);
    Mix_Volume(3, 8 * MIX_MAX_VOLUME / 10);
#ifdef __SWITCH__
    Mix_Volume(7, e->opt_voice * MIX_MAX_VOLUME / 10);
#else
    Mix_VolumeMusic(e->opt_voice * MIX_MAX_VOLUME / 10);
#endif
}

static Mix_Chunk *load_loop_chunk(Engine *e, int ch, const char *rel) {
    char full[MAX_PATH];
    Mix_Chunk **slot = (ch == 0) ? &e->ambient_chunk : &e->music_chunk;
    char *stored = (ch == 0) ? e->ambient_path : e->music_path;

    if (*slot && strcmp(stored, rel) == 0) {
        return *slot;
    }
    Mix_HaltChannel(ch);
    if (*slot) {
        Mix_FreeChunk(*slot);
        *slot = NULL;
    }
    join_path(full, sizeof(full), e->game_dir, rel);
    *slot = Mix_LoadWAV(full);
    if (!*slot) {
        c09_trace("audio: loop load failed: %s (%s)", full, Mix_GetError());
        stored[0] = 0;
        return NULL;
    }
    copy_str(stored, MAX_PATH, rel);
    return *slot;
}

static void play_chunk(Engine *e, const char *channel, const char *rel, double fade, int loop) {
    Mix_Chunk *chunk;
    int ch;
    if (!e->audio_ok || !rel || !rel[0]) {
        return;
    }
    ch = channel_index(channel);
    if (ch <= 1) {
        chunk = load_loop_chunk(e, ch, rel);
    } else {
        chunk = get_chunk(e, rel);
    }
    if (!chunk) {
        return;
    }
    Mix_HaltChannel(ch);
    if (fade > 0.01) {
        Mix_FadeInChannel(ch, chunk, loop ? -1 : 0, (int)(fade * 1000.0));
    } else {
        Mix_PlayChannel(ch, chunk, loop ? -1 : 0);
    }
    Mix_Volume(ch, (ch <= 1 ? e->opt_music : 8) * MIX_MAX_VOLUME / 10);
    if (ch <= 1) {
        c09_trace("audio: %s = %s (loop=%d)", channel, rel, loop);
    }
}

static void stop_channel(Engine *e, const char *channel, double fade) {
    int ch = channel_index(channel);
    if (e->audio_ok) {
        if (fade > 0.01) {
            Mix_FadeOutChannel(ch, (int)(fade * 1000.0));
        } else {
            Mix_HaltChannel(ch);
        }
    }
    if (ch == 0) {
        e->ambient_path[0] = 0;
    } else if (ch == 1) {
        e->music_path[0] = 0;
    }
}

static void ensure_save_dir(Engine *e);

static void stop_voice(Engine *e) {
#ifdef __SWITCH__
    if (e->audio_ok) {
        Mix_HaltChannel(7);
    }
#else
    if (e->audio_ok && e->voice_music) {
        Mix_HaltMusic();
    }
#endif
    e->voice_active = 0;
}

#ifdef __SWITCH__
/* Decode the scene voice track once and resample it to the mixer format.
 * Lines are then played straight from an offset in that PCM buffer, so
 * skipping never restarts or seeks an MP3 stream. */
static int decode_voice_pcm(const char *mp3, unsigned char **out, size_t *out_size) {    mp3dec_t decoder;
    mp3dec_file_info_t info;
    Sint16 *pcm;
    const Sint16 *src;
    size_t src_frames;
    size_t out_frames;
    size_t i;
    int channels;
    int rate;
    int c;
    int rc;

    memset(&info, 0, sizeof(info));
    mp3dec_init(&decoder);
    rc = mp3dec_load(&decoder, mp3, &info, NULL, NULL);
    if (rc != 0 || !info.buffer || info.samples == 0) {
        free(info.buffer);
        return 0;
    }
    channels = info.channels > 0 ? info.channels : 2;
    rate = info.hz > 0 ? info.hz : 44100;
    src_frames = info.samples / (size_t)channels;
    out_frames = (size_t)(((long long)src_frames * 44100LL) / (long long)rate);
    if (out_frames < 1) {
        out_frames = 1;
    }
    pcm = (Sint16 *)malloc(out_frames * 2u * sizeof(Sint16));
    if (!pcm) {
        free(info.buffer);
        return 0;
    }
    src = (const Sint16 *)info.buffer;
    for (i = 0; i < out_frames; i++) {
        double pos = (double)i * (double)rate / 44100.0;
        size_t i0 = (size_t)pos;
        size_t i1;
        double frac;
        if (i0 >= src_frames) {
            i0 = src_frames - 1;
        }
        i1 = i0 + 1;
        if (i1 >= src_frames) {
            i1 = src_frames - 1;
        }
        frac = pos - (double)i0;
        for (c = 0; c < 2; c++) {
            int ci = channels == 1 ? 0 : c;
            int s0 = src[i0 * (size_t)channels + (size_t)ci];
            int s1 = src[i1 * (size_t)channels + (size_t)ci];
            pcm[i * 2u + (size_t)c] = (Sint16)(s0 + (int)((double)(s1 - s0) * frac));
        }
    }
    free(info.buffer);
    *out = (unsigned char *)pcm;
    *out_size = out_frames * 2u * sizeof(Sint16);
    {
        const Sint16 *samples = (const Sint16 *)pcm;
        size_t count = out_frames * 2u;
        double sumsq = 0.0;
        size_t k;
        for (k = 0; k < count; k++) {
            double v = (double)samples[k];
            sumsq += v * v;
        }
        c09_trace("voice: pcm hz=%d ch=%d frames=%u out=%u sample_size=%u rms=%.1f first=%d,%d",
                  rate, channels, (unsigned)src_frames, (unsigned)*out_size,
                  (unsigned)sizeof(mp3d_sample_t), sqrt(sumsq / (double)(count ? count : 1)),
                  samples[0], samples[1]);
    }
    return 1;
}

static void voice_put16(unsigned char *p, unsigned value) {
    p[0] = (unsigned char)(value & 0xFF);
    p[1] = (unsigned char)((value >> 8) & 0xFF);
}

static void voice_put32(unsigned char *p, unsigned long value) {
    p[0] = (unsigned char)(value & 0xFF);
    p[1] = (unsigned char)((value >> 8) & 0xFF);
    p[2] = (unsigned char)((value >> 16) & 0xFF);
    p[3] = (unsigned char)((value >> 24) & 0xFF);
}

static void voice_wav_header(unsigned char *h, unsigned long data_bytes) {
    memcpy(h, "RIFF", 4);
    voice_put32(h + 4, 36ul + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    voice_put32(h + 16, 16);
    voice_put16(h + 20, 1);
    voice_put16(h + 22, 2);
    voice_put32(h + 24, 44100ul);
    voice_put32(h + 28, 44100ul * 4ul);
    voice_put16(h + 32, 4);
    voice_put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    voice_put32(h + 40, data_bytes);
}
#endif

static void play_voice(Engine *e, const char *rel, double start) {
    char full[MAX_PATH];
    if (!e->audio_ok || !rel || !rel[0]) {
        stop_voice(e);
        return;
    }
    join_path(full, sizeof(full), e->game_dir, rel);
#ifdef __SWITCH__
    if (!e->voice_pcm || strcmp(e->voice_pcm_path, full) != 0) {
        Uint32 decode_start = SDL_GetTicks();
        Mix_HaltChannel(7);
        free(e->voice_pcm);
        e->voice_pcm = NULL;
        e->voice_pcm_size = 0;
        e->voice_pcm_path[0] = 0;
        e->voice_pcm_silent = 0;
        if (!decode_voice_pcm(full, &e->voice_pcm, &e->voice_pcm_size)) {
            c09_trace("voice: decode failed %s", full);
            e->voice_active = 0;
            return;
        }
        copy_str(e->voice_pcm_path, sizeof(e->voice_pcm_path), full);
        c09_trace("voice: cached %s in %u ms", rel, (unsigned)(SDL_GetTicks() - decode_start));
        {
            const Sint16 *samples = (const Sint16 *)e->voice_pcm;
            size_t count = e->voice_pcm_size / 2u;
            size_t i;
            int silent = 1;
            for (i = 0; i < count; i++) {
                int v = samples[i];
                if (v > 64 || v < -64) {
                    silent = 0;
                    break;
                }
            }
            if (silent) {
                c09_trace("voice: track is silent, skipping playback");
                e->voice_pcm_silent = 1;
                e->voice_active = 0;
                return;
            }
            e->voice_pcm_silent = 0;
        }
    }
    if (e->voice_pcm_silent) {
        e->voice_active = 0;
        return;
    }
    {
        const size_t bytes_per_sec = 44100u * 2u * 2u;
        size_t offset = (size_t)(start * (double)bytes_per_sec);
        size_t length;
        /* The script offsets are milliseconds; a byte offset that is not a
         * whole 16-bit stereo frame makes the chunk noise. */
        offset -= offset & 3u;
        if (offset >= e->voice_pcm_size) {
            offset = 0;
        }
        if (e->line_end > start + 0.01) {
            length = (size_t)((e->line_end - start) * (double)bytes_per_sec);
        } else {
            length = e->voice_pcm_size - offset;
        }
        if (length > e->voice_pcm_size - offset) {
            length = e->voice_pcm_size - offset;
        }
        length -= length & 3u;
        if (length < 4) {
            e->voice_active = 0;
            return;
        }
        Mix_HaltMusic();
        if (e->voice_music) {
            Mix_FreeMusic(e->voice_music);
            e->voice_music = NULL;
        }
        {
            char wavpath[SAVE_PATH_MAX];
            FILE *file;
            int slot;
            snprintf(wavpath, sizeof(wavpath), "%s/voice_line.wav", e->save_dir);
            file = fopen(wavpath, "wb");
            if (file) {
                unsigned char header[44];
                voice_wav_header(header, (unsigned long)length);
                fwrite(header, 1, sizeof(header), file);
                fwrite(e->voice_pcm + offset, 1, length, file);
                fclose(file);
                slot = e->voice_chunk_slot;
                e->voice_chunk_slot = (slot + 1) & 3;
                Mix_HaltChannel(7);
                if (e->voice_chunks[slot]) {
                    Mix_FreeChunk(e->voice_chunks[slot]);
                    e->voice_chunks[slot] = NULL;
                }
                e->voice_chunks[slot] = Mix_LoadWAV(wavpath);
                if (e->voice_chunks[slot] && Mix_PlayChannel(7, e->voice_chunks[slot], 0) >= 0) {
                    e->voice_active = 1;
                } else {
                    e->voice_active = 0;
                }
            }
        }
        Mix_Volume(7, e->opt_voice * MIX_MAX_VOLUME / 10);
    }
#else
    if (!e->voice_music || strcmp(e->voice_music_path, full) != 0) {
        if (e->voice_music) {
            Mix_HaltMusic();
            Mix_FreeMusic(e->voice_music);
            e->voice_music = NULL;
        }
        e->voice_music = Mix_LoadMUS(full);
        copy_str(e->voice_music_path, sizeof(e->voice_music_path), full);
    }
    if (!e->voice_music) {
        e->voice_active = 0;
        return;
    }
    Mix_HaltMusic();
    if (Mix_PlayMusic(e->voice_music, 0) == 0) {
        if (start > 0.01) {
            Mix_SetMusicPosition(start);
        }
        e->voice_active = 1;
    } else {
        e->voice_active = 0;
    }
    Mix_VolumeMusic(e->opt_voice * MIX_MAX_VOLUME / 10);
#endif
}

/* ------------------------------------------------------------------------- */
/* save files                                                                */
/* ------------------------------------------------------------------------- */

static void write_escaped(FILE *file, const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        switch (*p) {
        case '\\':
            fputs("\\\\", file);
            break;
        case '\t':
            fputs("\\t", file);
            break;
        case '\n':
            fputs("\\n", file);
            break;
        case '\r':
            fputs("\\r", file);
            break;
        default:
            fputc(*p, file);
            break;
        }
        p++;
    }
}

static void unescape_into(char *dst, size_t n, const char *src) {
    size_t out = 0;
    if (!n) {
        return;
    }
    if (!src) {
        dst[0] = 0;
        return;
    }
    while (*src && out + 1 < n) {
        char ch = *src++;
        if (ch == '\\' && *src) {
            char esc = *src++;
            switch (esc) {
            case 't':
                ch = '\t';
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case '\\':
                ch = '\\';
                break;
            default:
                ch = esc;
                break;
            }
        }
        dst[out++] = ch;
    }
    dst[out] = 0;
}

static void ensure_save_dir(Engine *e) {
#ifdef __PROSPERO__
    /* /download0 is the app's writable area; create it and the saves folder
     * on first run. Failures are harmless: saves simply become unavailable. */
    MKDIR("/download0");
#elif defined(__SWITCH__)
    MKDIR("sdmc:/switch");
    MKDIR("sdmc:/switch/class09");
#endif
    MKDIR(e->save_dir);
}

static void slot_path(Engine *e, int slot, char *out, size_t n) {
    snprintf(out, n, "%s/slot%02d.sav", e->save_dir, slot);
}

static int slot_exists(Engine *e, int slot) {
    char path[SAVE_PATH_MAX];
    FILE *file;
    slot_path(e, slot, path, sizeof(path));
    file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    fclose(file);
    return 1;
}

static long slot_time(Engine *e, int slot) {
    char path[SAVE_PATH_MAX];
    char raw[512];
    FILE *file;
    long value = -1;
    slot_path(e, slot, path, sizeof(path));
    file = fopen(path, "rb");
    if (!file) {
        return -1;
    }
    while (fgets(raw, sizeof(raw), file)) {
        if (strncmp(raw, "saved_at\t", 9) == 0) {
            value = atol(raw + 9);
            break;
        }
    }
    fclose(file);
    return value;
}

static int latest_slot(Engine *e) {
    int i;
    int best = -1;
    long best_time = -1;
    for (i = 0; i < MAX_SLOTS; i++) {
        long when = slot_time(e, i);
        if (when >= 0 && when >= best_time) {
            best_time = when;
            best = i;
        }
    }
    return best;
}

static void thumbnail_path(Engine *e, int slot, char *out, size_t n) {
    snprintf(out, n, "%s/slot%02d.bmp", e->save_dir, slot);
}

static void pause_thumbnail_path(Engine *e, char *out, size_t n) {
    snprintf(out, n, "%s/pause_frame.bmp", e->save_dir);
}

static int copy_file(const char *src, const char *dst) {
    FILE *in;
    FILE *out;
    char buffer[8192];
    size_t got;
    in = fopen(src, "rb");
    if (!in) {
        return 0;
    }
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, got, out) != got) {
            fclose(in);
            fclose(out);
            return 0;
        }
    }
    fclose(in);
    fclose(out);
    return 1;
}

static void save_slot_thumbnail(Engine *e, int slot) {
    char source[SAVE_PATH_MAX];
    char dest[SAVE_PATH_MAX];
    pause_thumbnail_path(e, source, sizeof(source));
    thumbnail_path(e, slot, dest, sizeof(dest));
    copy_file(source, dest);
}

static int slot_thumbnail_exists(Engine *e, int slot) {
    char path[SAVE_PATH_MAX];
    FILE *file;
    thumbnail_path(e, slot, path, sizeof(path));
    file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    fclose(file);
    return 1;
}

static void format_slot_time(long when, char *out, size_t n) {
    time_t stamp = (time_t)when;
    struct tm *parts = localtime(&stamp);
    if (!parts || strftime(out, n, "%A, %B %d %Y, %H:%M", parts) == 0) {
        snprintf(out, n, "Unknown time");
    }
}

static void preview_slot(Engine *e, int slot, char *out, size_t n) {
    char path[SAVE_PATH_MAX];
    char raw[MAX_TEXT + 1024];
    char speaker[128];
    char line[MAX_TEXT];
    FILE *file;
    size_t len;

    speaker[0] = 0;
    line[0] = 0;
    slot_path(e, slot, path, sizeof(path));
    file = fopen(path, "rb");
    if (!file) {
        copy_str(out, n, "empty");
        return;
    }
    while (fgets(raw, sizeof(raw), file)) {
        if (strncmp(raw, "speaker\t", 8) == 0) {
            unescape_into(speaker, sizeof(speaker), raw + 8);
        } else if (strncmp(raw, "line\t", 5) == 0) {
            unescape_into(line, sizeof(line), raw + 5);
        }
    }
    fclose(file);
    if (speaker[0]) {
        snprintf(out, n, "%s: %s", speaker, line);
    } else {
        copy_str(out, n, line);
    }
    len = strlen(out);
    if (len > 86) {
        out[86] = 0;
        out[85] = '.';
        out[84] = '.';
        out[83] = '.';
    }
}

static int write_field_str(FILE *file, const char *key, const char *value) {
    fputs(key, file);
    fputc('\t', file);
    write_escaped(file, value);
    fputc('\n', file);
    return 1;
}

static int save_game(Engine *e, int slot) {
    char path[SAVE_PATH_MAX];
    char when[64];
    FILE *file;
    int i;

    ensure_save_dir(e);
    slot_path(e, slot, path, sizeof(path));
    file = fopen(path, "wb");
    if (!file) {
        return 0;
    }
    snprintf(when, sizeof(when), "%ld", (long)time(NULL));
    fputs("CLASS09SAVE\t3\n", file);
    write_field_str(file, "saved_at", when);
    fprintf(file, "pc\t%d\n", e->pc);
    fprintf(file, "wait_kind\t%d\n", e->wait_kind);
    fprintf(file, "waiting\t%d\n", e->waiting);
    write_field_str(file, "speaker", e->speaker);
    write_field_str(file, "line", e->line);
    write_field_str(file, "voice", e->voice);
    write_field_str(file, "ambient", e->ambient_path);
    write_field_str(file, "music", e->music_path);
    fprintf(file, "hud\t%d\n", e->hud);
    fprintf(file, "choice_count\t%d\n", e->choice_count);
    fprintf(file, "choice_index\t%d\n", e->choice_index);
    for (i = 0; i < e->choice_count; i++) {
        fputs("choice\t", file);
        write_escaped(file, e->choice_text[i]);
        fprintf(file, "\t%d\n", e->choice_targets[i]);
    }
    for (i = 0; i < MAX_ITEMS; i++) {
        LayerItem *item = &e->items[i];
        if (!item->used) {
            continue;
        }
        fputs("item\t", file);
        write_escaped(file, item->layer);
        fputc('\t', file);
        write_escaped(file, item->tag);
        fputc('\t', file);
        write_escaped(file, item->kind);
        fputc('\t', file);
        write_escaped(file, item->rel);
        fputc('\t', file);
        write_escaped(file, item->arg);
        fprintf(file, "\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", item->x, item->y, item->w, item->h, item->alpha, item->flip,
                item->order);
    }
    fputs("end\n", file);
    fclose(file);
    save_slot_thumbnail(e, slot);
    return 1;
}

static int split_fields(char *line, char **fields, int max_fields) {
    int count = 0;
    char *p = line;
    while (count < max_fields) {
        char *tab;
        fields[count++] = p;
        tab = strchr(p, '\t');
        if (!tab) {
            break;
        }
        *tab = 0;
        p = tab + 1;
    }
    return count;
}

static void clear_items(Engine *e);

static void history_reset(Engine *e);

static int load_game(Engine *e, int slot) {
    char path[SAVE_PATH_MAX];
    char raw[MAX_TEXT + 1024];
    char ambient[MAX_PATH];
    char music[MAX_PATH];
    FILE *file;
    int waiting = 0;
    int choice_write = 0;

    slot_path(e, slot, path, sizeof(path));
    file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    if (!fgets(raw, sizeof(raw), file) || strncmp(raw, "CLASS09SAVE\t3", 13) != 0) {
        fclose(file);
        printf("class09: ignoring save slot %d written by an older script\n", slot);
        return 0;
    }
    stop_voice(e);
    stop_channel(e, "ambient", 0);
    stop_channel(e, "music", 0);
    clear_items(e);
    e->choice_count = 0;
    e->choice_index = 0;
    e->waiting = 0;
    e->wait_auto = 0;
    e->wait_kind = WAIT_NONE;
    e->ending_active = 0;
    ambient[0] = 0;
    music[0] = 0;

    while (fgets(raw, sizeof(raw), file)) {
        char *fields[16];
        int count;
        size_t len = strlen(raw);
        while (len > 0 && (raw[len - 1] == '\n' || raw[len - 1] == '\r')) {
            raw[--len] = 0;
        }
        if (!len) {
            continue;
        }
        count = split_fields(raw, fields, 16);
        if (count < 2) {
            continue;
        }
        if (strcmp(fields[0], "pc") == 0) {
            e->pc = atoi(fields[1]);
        } else if (strcmp(fields[0], "wait_kind") == 0) {
            e->wait_kind = atoi(fields[1]);
        } else if (strcmp(fields[0], "speaker") == 0) {
            unescape_into(e->speaker, sizeof(e->speaker), fields[1]);
        } else if (strcmp(fields[0], "line") == 0) {
            unescape_into(e->line, sizeof(e->line), fields[1]);
        } else if (strcmp(fields[0], "voice") == 0) {
            unescape_into(e->voice, sizeof(e->voice), fields[1]);
        } else if (strcmp(fields[0], "ambient") == 0) {
            unescape_into(ambient, sizeof(ambient), fields[1]);
        } else if (strcmp(fields[0], "music") == 0) {
            unescape_into(music, sizeof(music), fields[1]);
        } else if (strcmp(fields[0], "hud") == 0) {
            e->hud = atoi(fields[1]);
        } else if (strcmp(fields[0], "waiting") == 0) {
            waiting = atoi(fields[1]);
        } else if (strcmp(fields[0], "choice_index") == 0) {
            e->choice_index = atoi(fields[1]);
        } else if (strcmp(fields[0], "choice_count") == 0) {
            e->choice_count = atoi(fields[1]);
            if (e->choice_count > MAX_CHOICES) {
                e->choice_count = MAX_CHOICES;
            }
        } else if (strcmp(fields[0], "choice") == 0 && count >= 3) {
            if (choice_write < MAX_CHOICES) {
                unescape_into(e->choice_text[choice_write], sizeof(e->choice_text[0]), fields[1]);
                e->choice_targets[choice_write] = atoi(fields[2]);
                choice_write++;
            }
        } else if (strcmp(fields[0], "item") == 0 && count >= 12) {
            int index = -1;
            int order = 0;
            int j;
            for (j = 0; j < MAX_ITEMS; j++) {
                if (!e->items[j].used) {
                    index = j;
                    break;
                }
            }
            if (index >= 0) {
                LayerItem *item = &e->items[index];
                memset(item, 0, sizeof(*item));
                item->used = 1;
                unescape_into(item->layer, sizeof(item->layer), fields[1]);
                unescape_into(item->tag, sizeof(item->tag), fields[2]);
                unescape_into(item->kind, sizeof(item->kind), fields[3]);
                unescape_into(item->rel, sizeof(item->rel), fields[4]);
                unescape_into(item->arg, sizeof(item->arg), fields[5]);
                item->x = atoi(fields[6]);
                item->y = atoi(fields[7]);
                item->w = atoi(fields[8]);
                item->h = atoi(fields[9]);
                item->alpha = atoi(fields[10]);
                item->flip = atoi(fields[11]);
                if (count >= 13) {
                    order = atoi(fields[12]);
                }
                /* Older saves have no order field: fall back to file order. */
                item->order = order > 0 ? order : ++e->next_order;
                if (item->order > e->next_order) {
                    e->next_order = item->order;
                }
                e->item_count++;
            }
        }
    }
    fclose(file);

    e->choice_count = choice_write;
    if (e->choice_index < 0) {
        e->choice_index = 0;
    }
    if (e->choice_count > 0 && e->choice_index >= e->choice_count) {
        e->choice_index = e->choice_count - 1;
    }
    e->line_shown = e->line[0] != 0;
    e->wait_auto = 0;
    e->wait_deadline = 0;
    if (e->wait_kind == WAIT_MENU && e->choice_count <= 0) {
        e->wait_kind = WAIT_NONE;
    }
    /* Timed transitions already incremented pc before the save; do not freeze on them. */
    if (e->wait_kind == WAIT_PAUSE || e->wait_kind == WAIT_MOVIE) {
        e->wait_kind = WAIT_NONE;
        waiting = 0;
    }
    e->waiting = (waiting || e->wait_kind != WAIT_NONE) ? 1 : 0;
    if (!e->waiting) {
        e->wait_kind = WAIT_NONE;
    }

    copy_str(e->ambient_path, sizeof(e->ambient_path), ambient);
    copy_str(e->music_path, sizeof(e->music_path), music);
    if (ambient[0]) {
        char temp[MAX_PATH];
        copy_str(temp, sizeof(temp), ambient);
        play_chunk(e, "ambient", temp, 0, 1);
    }
    if (music[0]) {
        char temp[MAX_PATH];
        copy_str(temp, sizeof(temp), music);
        play_chunk(e, "music", temp, 0, 1);
    }
    apply_volumes(e);
    history_reset(e);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* persistent ending tracker                                                 */
/* ------------------------------------------------------------------------- */

static void save_seen(Engine *e) {
    char path[SAVE_PATH_MAX];
    FILE *file;
    int i;
    ensure_save_dir(e);
    snprintf(path, sizeof(path), "%s/seen.txt", e->save_dir);
    file = fopen(path, "wb");
    if (!file) {
        return;
    }
    for (i = 0; i < e->seen_count; i++) {
        fprintf(file, "%s\n", e->seen[i]);
    }
    fclose(file);
}

static void load_seen(Engine *e) {
    char path[SAVE_PATH_MAX];
    char raw[128];
    FILE *file;
    snprintf(path, sizeof(path), "%s/seen.txt", e->save_dir);
    file = fopen(path, "rb");
    if (!file) {
        return;
    }
    while (fgets(raw, sizeof(raw), file) && e->seen_count < MAX_SEEN) {
        size_t len = strlen(raw);
        while (len > 0 && (raw[len - 1] == '\n' || raw[len - 1] == '\r')) {
            raw[--len] = 0;
        }
        if (len == 4) {
            copy_str(e->seen[e->seen_count], sizeof(e->seen[0]), raw);
            e->seen_count++;
        }
    }
    fclose(file);
}

static int ending_is_seen(Engine *e, const char *id) {
    int i;
    for (i = 0; i < e->seen_count; i++) {
        if (strcmp(e->seen[i], id) == 0) {
            return 1;
        }
    }
    return 0;
}

static void save_new_ending(Engine *e) {
    char path[SAVE_PATH_MAX];
    FILE *file;
    ensure_save_dir(e);
    snprintf(path, sizeof(path), "%s/new_ending", e->save_dir);
    file = fopen(path, "wb");
    if (!file) {
        return;
    }
    fputs(e->new_ending ? "1\n" : "0\n", file);
    fclose(file);
}

static void load_new_ending(Engine *e) {
    char path[SAVE_PATH_MAX];
    char raw[16];
    FILE *file;
    snprintf(path, sizeof(path), "%s/new_ending", e->save_dir);
    file = fopen(path, "rb");
    if (!file) {
        return;
    }
    if (fgets(raw, sizeof(raw), file)) {
        e->new_ending = raw[0] == '1';
    }
    fclose(file);
}

static void mark_seen(Engine *e, const char *id) {
    if (!id || strlen(id) != 4 || ending_is_seen(e, id)) {
        return;
    }
    if (e->seen_count < MAX_SEEN) {
        copy_str(e->seen[e->seen_count], sizeof(e->seen[0]), id);
        e->seen_count++;
        save_seen(e);
        e->new_ending = 1;
        save_new_ending(e);
        c09_trace("seen: %s count=%d", id, e->seen_count);
    }
}

/* ------------------------------------------------------------------------- */
/* staging helpers                                                           */
/* ------------------------------------------------------------------------- */

static void clear_layer(Engine *e, const char *layer) {
    int i;
    for (i = 0; i < MAX_ITEMS; i++) {
        if (e->items[i].used && strcmp(e->items[i].layer, layer) == 0) {
            e->items[i].used = 0;
        }
    }
}

static void clear_items(Engine *e) {
    int i;
    for (i = 0; i < MAX_ITEMS; i++) {
        e->items[i].used = 0;
    }
    e->item_count = 0;
    e->next_order = 0;
}

static int find_item(Engine *e, const char *tag, const char *layer) {
    int i;
    if (!tag || !tag[0]) {
        return -1;
    }
    for (i = 0; i < MAX_ITEMS; i++) {
        if (e->items[i].used && strcmp(e->items[i].tag, tag) == 0 && strcmp(e->items[i].layer, layer) == 0) {
            return i;
        }
    }
    return -1;
}

static void set_item(Engine *e, const char *rel, const char *tag, const char *layer, const char *kind, const char *arg,
                     int x, int y, int w, int h, int alpha, int flip, JsonValue *frames) {
    int index = find_item(e, tag, layer);
    int order = 0;
    int i;
    int keep_position = 0;
    int keep_x = 0;
    int keep_y = 0;
    LayerItem *item;

    if (index >= 0) {
        order = e->items[index].order;
        /* A bare `show <tag>` keeps Ren'Py's current transform, but the
         * exporter re-emits the default center.  Keep the gamer brother at
         * his existing spot while Mom is in the scene so he does not slide on
         * top of Nicole and Mom; the next scene stages him normally. */
        if (strcmp(tag, "gamer_brother") == 0 && find_item(e, "mom", layer) >= 0) {
            keep_position = 1;
            keep_x = e->items[index].x;
            keep_y = e->items[index].y;
        }
    }
    if (index < 0) {
        for (i = 0; i < MAX_ITEMS; i++) {
            if (!e->items[i].used) {
                index = i;
                break;
            }
        }
    }
    if (index < 0) {
        index = 0;
    }
    item = &e->items[index];
    memset(item, 0, sizeof(*item));
    item->used = 1;
    copy_str(item->tag, sizeof(item->tag), tag ? tag : "");
    copy_str(item->layer, sizeof(item->layer), layer && layer[0] ? layer : "master");
    copy_str(item->kind, sizeof(item->kind), kind ? kind : "none");
    copy_str(item->rel, sizeof(item->rel), rel ? rel : "");
    copy_str(item->arg, sizeof(item->arg), arg ? arg : "");
    item->x = keep_position ? keep_x : x;
    item->y = keep_position ? keep_y : y;
    item->w = w;
    item->h = h;
    item->alpha = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
    item->flip = flip;
    item->order = order ? order : ++e->next_order;
    item->anim_count = 0;
    item->anim_paused = 0;
    frames = anim_frames(frames);
    if (frames && frames->u.array.count >= 2) {
        size_t total = frames->u.array.count;
        for (i = 0; i < (int)total && item->anim_count < MAX_ANIM; i++) {
            JsonValue *frame = frames->u.array.items[i];
            int k;
            if (!frame || frame->type != JL_ARRAY || frame->u.array.count < 7) {
                continue;
            }
            for (k = 0; k < 7; k++) {
                item->anim[item->anim_count][k] = (int)json_as_number(frame->u.array.items[k]);
            }
            item->anim_count++;
        }
        if (item->anim_count >= 2) {
            item->anim_start = SDL_GetTicks();
        }
    }
    if (index >= e->item_count) {
        e->item_count = index + 1;
    }
}

static void hide_item(Engine *e, const char *tag, const char *layer) {
    int i;
    for (i = 0; i < MAX_ITEMS; i++) {
        LayerItem *item = &e->items[i];
        if (!item->used) {
            continue;
        }
        if (layer && layer[0] && strcmp(item->layer, layer) != 0) {
            continue;
        }
        if (tag && tag[0] && strcmp(item->tag, tag) != 0) {
            continue;
        }
        item->used = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* conditions / endings                                                      */
/* ------------------------------------------------------------------------- */

static int eval_cond(Engine *e, const char *cond) {
    const char *p;
    char id[8];
    int has_id = 0;
    if (!cond || !cond[0] || strcmp(cond, "True") == 0 || strcmp(cond, "true") == 0) {
        return 1;
    }
    if (strcmp(cond, "False") == 0 || strcmp(cond, "false") == 0) {
        return 0;
    }
    for (p = cond; p[0] && p[1] && p[2] && p[3]; p++) {
        if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]) &&
            isdigit((unsigned char)p[3])) {
            id[0] = p[0];
            id[1] = p[1];
            id[2] = p[2];
            id[3] = p[3];
            id[4] = 0;
            has_id = 1;
            break;
        }
    }
    if (!has_id) {
        return 1;
    }
    if (strstr(cond, "not in")) {
        return !ending_is_seen(e, id);
    }
    return ending_is_seen(e, id);
}

static int load_movie(Engine *e, const char *subdir, int mode);
#if defined(__PROSPERO__) || defined(__SWITCH__)
static int load_webm_movie(Engine *e, const char *path, int mode);
#endif
static int load_ending_video(Engine *e, const char *id);

static void begin_ending(Engine *e, const char *id) {
    int i;
    int count;
    copy_str(e->ending_title, sizeof(e->ending_title), "Ending");
    e->ending_body[0] = 0;
    if (e->endings && e->endings->type == JL_ARRAY) {
        count = (int)e->endings->u.array.count;
        for (i = 0; i < count; i++) {
            JsonValue *item = e->endings->u.array.items[i];
            const char *item_id = obj_str(item, "id", NULL);
            if (item_id && strcmp(item_id, id) == 0) {
                copy_str(e->ending_title, sizeof(e->ending_title), obj_str(item, "title", "Ending"));
                copy_str(e->ending_body, sizeof(e->ending_body), obj_str(item, "text", ""));
                break;
            }
        }
    }
    e->ending_active = 1;
    e->line_shown = 0;
    stop_voice(e);
    stop_channel(e, "ambient", 0);
    stop_channel(e, "music", 0);
    if (id && id[0]) {
        c09_trace("ending: id=%s text=%s", id, e->ending_title);
        if (load_ending_video(e, id)) {
            c09_trace("ending: video for %s started", id);
            e->waiting = 0;
            e->wait_auto = 0;
            e->wait_kind = WAIT_NONE;
            return;
        }
        c09_trace("ending: no video for %s", id);
    }
    e->waiting = 1;
    e->wait_auto = 0;
    e->wait_kind = WAIT_ENDING;
}

/* ------------------------------------------------------------------------- */
/* screen transitions                                                        */
/* ------------------------------------------------------------------------- */

static void stop_percpop(Engine *e);

static void enter_main_menu(Engine *e) {
    e->mode = MODE_MENU;
    if (e->audio_ok) {
        Mix_Resume(-1);
        Mix_ResumeMusic();
    }
    stop_voice(e);
    stop_channel(e, "ambient", 0);
    stop_channel(e, "music", 0);
    clear_items(e);
    e->choice_count = 0;
    e->choice_index = 0;
    e->waiting = 0;
    e->wait_auto = 0;
    e->wait_kind = WAIT_NONE;
    e->ending_active = 0;
    history_reset(e);
    stop_percpop(e);
    e->line_shown = 0;
    e->line[0] = 0;
    e->speaker[0] = 0;
    e->main_index = 0;
    c09_trace("mode: main menu");
    if (e->gui_menu_music[0] && !e->splash_active) {
        play_chunk(e, "music", e->gui_menu_music, 0, 1);
    }
    if (e->new_ending) {
        e->newmsg_active = 1;
        e->newmsg_frame = 0;
        e->newmsg_next = SDL_GetTicks();
        e->newmsg_skip_at = e->newmsg_next + 900;
        play_chunk(e, "sfx", "audio/NewTextAlert.mp3", 0, 0);
    } else {
        e->newmsg_active = 0;
    }
}

static void enter_endings(Engine *e) {
    e->mode = MODE_ENDINGS;
    e->endings_index = 0;
    e->endings_detail = 0;
    e->endings_scroll = 0;
    e->endings_top = 0;
    e->newmsg_active = 0;
    if (e->new_ending) {
        e->new_ending = 0;
        save_new_ending(e);
    }
    play_chunk(e, "sfx", "audio/Phone_slide.mp3", 0, 0);
}

static void start_game(Engine *e) {
    const char *start = obj_str(e->root, "start", "start");
    int index = label_index(e, start);
    if (index < 0) {
        index = label_index(e, "start");
    }
    stop_voice(e);
    stop_channel(e, "ambient", 0);
    stop_channel(e, "music", 0);
    clear_items(e);
    e->choice_count = 0;
    e->choice_index = 0;
    e->waiting = 0;
    e->wait_auto = 0;
    e->wait_kind = WAIT_NONE;
    e->line[0] = 0;
    e->speaker[0] = 0;
    e->line_shown = 0;
    e->voice[0] = 0;
    e->ending_active = 0;
    e->newmsg_active = 0;
    history_reset(e);
    stop_percpop(e);
    e->hud = 1;
    e->pc = index >= 0 ? index : 0;
    e->mode = MODE_GAME;
    c09_trace("mode: game start label=%s pc=%d", start, e->pc);
}

static void resume_game(Engine *e) {
    Uint32 now = SDL_GetTicks();
    Uint32 paused = now - e->pause_started;
    int i;
    e->mode = MODE_GAME;
    c09_trace("mode: game resume pc=%d", e->pc);
    if (e->audio_ok) {
        Mix_Resume(0);
        Mix_Resume(1);
        Mix_ResumeMusic();
    }
    for (i = 0; i < MAX_ITEMS; i++) {
        LayerItem *item = &e->items[i];
        if (item->used && item->anim_paused) {
            item->anim_start += paused;
            item->anim_paused = 0;
        }
    }
    if (e->waiting && e->wait_auto && e->wait_deadline >= e->pause_started) {
        e->wait_deadline += paused;
    }
}

static void enter_pause(Engine *e) {
    Uint32 now = SDL_GetTicks();
    int i;
    e->pause_started = now;
    e->pause_index = 0;
    e->want_thumb = 1;
    for (i = 0; i < MAX_ITEMS; i++) {
        LayerItem *item = &e->items[i];
        if (!item->used || item->anim_count < 2) {
            continue;
        }
        if ((Uint32)(now - item->anim_start) < (Uint32)item->anim[item->anim_count - 1][0]) {
            item->anim_paused = 1;
        }
    }
    e->mode = MODE_PAUSE;
    c09_trace("mode: pause pc=%d", e->pc);
    if (e->audio_ok) {
        Mix_Pause(0);
        Mix_Pause(1);
        Mix_PauseMusic();
    }
}

static void enter_options(Engine *e, int return_mode) {
    e->options_return_mode = return_mode;
    e->options_index = 0;
    e->mode = MODE_OPTIONS;
    c09_trace("mode: options from=%d", return_mode);
}

static void enter_load(Engine *e, int return_mode) {
    e->load_return_mode = return_mode;
    e->load_index = 0;
    e->file_page = 0;
    e->file_area = 0;
    e->page_focus = 1;
    e->mode = MODE_LOAD;
    c09_trace("mode: load from=%d", return_mode);
}

static void enter_save(Engine *e, int return_mode) {
    e->save_return_mode = return_mode;
    e->save_index = 0;
    e->load_index = 0;
    e->file_page = 0;
    e->file_area = 0;
    e->page_focus = 1;
    e->mode = MODE_SAVE;
    c09_trace("mode: save from=%d", return_mode);
}

/* ------------------------------------------------------------------------- */
/* bytecode interpreter                                                      */
/* ------------------------------------------------------------------------- */

static void select_choice(Engine *e) {
    int index = e->choice_index;
    if (index >= 0 && index < e->choice_count) {
        play_chunk(e, "sfx", "audio/PhoneSelect.mp3", 0, 0);
        e->pc = e->choice_targets[index];
        c09_trace("menu: chose %d -> pc=%d", index, e->pc);
        e->waiting = 0;
        e->wait_auto = 0;
        e->wait_kind = WAIT_NONE;
        e->choice_count = 0;
        e->choice_index = 0;
    }
}

static int load_intro(Engine *e);
static int load_percpop(Engine *e);
static void stop_percpop(Engine *e);
static void draw_percpop(Engine *e);

static void history_reset(Engine *e) { e->history_pos = 0; }

static void history_push(Engine *e, int pc) {
    HistoryEntry *entry;
    if (e->history_pos > 0 && e->history[e->history_pos - 1].pc == pc) {
        return; /* re-executed after a rollback: keep the older entries */
    }
    if (e->history_pos >= HISTORY_MAX) {
        memmove(&e->history[0], &e->history[1], sizeof(HistoryEntry) * (HISTORY_MAX - 1));
        e->history_pos = HISTORY_MAX - 1;
    }
    entry = &e->history[e->history_pos++];
    entry->pc = pc;
    entry->hud = e->hud;
    entry->next_order = e->next_order;
    entry->item_count = e->item_count;
    entry->ending_active = e->ending_active;
    memcpy(entry->items, e->items, sizeof(e->items));
}

static int history_back(Engine *e) {
    HistoryEntry *entry;
    if (e->history_pos <= 1) {
        return 0;
    }
    e->history_pos--;
    entry = &e->history[e->history_pos - 1];
    e->pc = entry->pc;
    e->hud = entry->hud;
    e->next_order = entry->next_order;
    e->item_count = entry->item_count;
    e->ending_active = entry->ending_active;
    memcpy(e->items, entry->items, sizeof(e->items));
    e->waiting = 0;
    e->wait_auto = 0;
    e->wait_deadline = 0;
    e->wait_kind = WAIT_NONE;
    e->choice_count = 0;
    e->choice_index = 0;
    e->line_shown = 0;
    e->line[0] = 0;
    e->speaker[0] = 0;
    stop_voice(e);
    c09_trace("rollback: pc=%d", e->pc);
    return 1;
}

static void execute_op(Engine *e) {
    JsonValue *op;
    const char *kind;

    if (!e->ops || e->ops->type != JL_ARRAY || e->pc < 0 || (size_t)e->pc >= e->ops->u.array.count) {
        enter_main_menu(e);
        return;
    }
    op = e->ops->u.array.items[e->pc];
    kind = op_str(op, 0);

    if (strcmp(kind, "label") == 0) {
        e->pc++;
        return;
    }
    if (strcmp(kind, "say") == 0) {
        const char *voice;
        double start = op_num(op, 3);
        double end = op_num(op, 4);
        double duration = (end > start && end > 0.0) ? end - start : 0.0;
        history_push(e, e->pc);
        e->pc++;
        copy_str(e->speaker, sizeof(e->speaker), op_str(op, 1));
        copy_str(e->line, sizeof(e->line), op_str(op, 2));
        e->line_shown = 1;
        e->waiting = 1;
        e->wait_kind = WAIT_SAY;
        e->line_start = start;
        e->line_end = end;
        if (duration > 0.01) {
            e->wait_auto = 1;
            e->wait_deadline = SDL_GetTicks() + (Uint32)(duration * 1000.0);
        } else {
            e->wait_auto = 0;
            e->wait_deadline = 0;
        }
        voice = op_str(op, 5);
        if (!voice[0]) {
            voice = e->voice;
        }
        if (voice[0]) {
            play_voice(e, voice, start > 0.0 ? start : 0.0);
        } else {
            stop_voice(e);
        }
        return;
    }
    if (strcmp(kind, "scene") == 0) {
        const char *scene_kind = op_str(op, 2);
        e->pc++;
        clear_layer(e, "master");
        if (scene_kind[0] && strcmp(scene_kind, "none") != 0) {
            set_item(e, op_str(op, 1), "_scene", "master", scene_kind, op_str(op, 3), op_int(op, 4), op_int(op, 5),
                     op_int(op, 6), op_int(op, 7), (int)(op_num(op, 8) * 255.0 + 0.5), op_int(op, 9), op_item(op, 10));
        }
        return;
    }
    if (strcmp(kind, "show") == 0) {
        const char *item_kind = op_str(op, 4);
        const char *item_arg = op_str(op, 5);
        e->pc++;
        if (strcmp(item_kind, "movie") == 0 && strstr(item_arg, "percpop")) {
            JsonValue *frames = anim_frames(op_item(op, 12));
            int animated = frames && frames->u.array.count > 0;
            /* The second percpop show has alpha 0 and no keyframes: stay hidden. */
            if (animated || op_num(op, 10) > 0.01) {
                load_percpop(e);
            }
            return;
        }
        if (strcmp(item_kind, "movie") == 0 && !e->splash_active && item_arg[0] &&
            (strstr(item_arg, "opening") || strstr(item_arg, "intro")) && load_intro(e)) {
            e->waiting = 0;
            e->wait_auto = 0;
            e->wait_kind = WAIT_NONE;
            return;
        }
        set_item(e, op_str(op, 1), op_str(op, 2), op_str(op, 3), item_kind, item_arg, op_int(op, 6),
                 op_int(op, 7), op_int(op, 8), op_int(op, 9), (int)(op_num(op, 10) * 255.0 + 0.5), op_int(op, 11),
                 op_item(op, 12));
        return;
    }
    if (strcmp(kind, "hide") == 0) {
        e->pc++;
        hide_item(e, op_str(op, 1), op_str(op, 2));
        return;
    }
    if (strcmp(kind, "voice") == 0) {
        copy_str(e->voice, sizeof(e->voice), op_str(op, 1));
        e->pc++;
        return;
    }
    if (strcmp(kind, "play") == 0) {
        play_chunk(e, op_str(op, 1), op_str(op, 2), op_num(op, 3), op_int(op, 4));
        e->pc++;
        return;
    }
    if (strcmp(kind, "stop") == 0) {
        stop_channel(e, op_str(op, 1), op_num(op, 2));
        e->pc++;
        return;
    }
    if (strcmp(kind, "pause") == 0) {
        double seconds = op_num(op, 1);
        e->pc++;
        if (seconds > 0.01) {
            e->waiting = 1;
            e->wait_auto = 1;
            e->wait_kind = WAIT_PAUSE;
            e->wait_deadline = SDL_GetTicks() + (Uint32)(seconds * 1000.0);
        }
        return;
    }
    if (strcmp(kind, "movie") == 0) {
        e->pc++;
        e->waiting = 1;
        e->wait_auto = 1;
        e->wait_kind = WAIT_MOVIE;
        e->wait_deadline = SDL_GetTicks() + 1200;
        return;
    }
    if (strcmp(kind, "jump") == 0) {
        int index = label_index(e, op_str(op, 1));
        if (index >= 0) {
            e->pc = index;
        } else {
            e->pc++;
        }
        return;
    }
    if (strcmp(kind, "jump_abs") == 0) {
        e->pc = op_int(op, 1);
        return;
    }
    if (strcmp(kind, "call") == 0) {
        int index = label_index(e, op_str(op, 1));
        if (e->call_depth < CALL_STACK) {
            e->call_stack[e->call_depth++] = e->pc + 1;
        }
        if (index >= 0) {
            e->pc = index;
        } else {
            e->pc++;
        }
        return;
    }
    if (strcmp(kind, "return") == 0) {
        if (e->call_depth > 0) {
            e->pc = e->call_stack[--e->call_depth];
        } else {
            enter_main_menu(e);
        }
        return;
    }
    if (strcmp(kind, "menu") == 0) {
        JsonValue *choices = op_item(op, 1);
        int i;
        e->pc++;
        e->choice_count = 0;
        e->choice_index = 0;
        if (choices && choices->type == JL_ARRAY) {
            size_t total = choices->u.array.count;
            for (i = 0; i < (int)total; i++) {
                JsonValue *choice = choices->u.array.items[i];
                if (!choice || choice->type != JL_ARRAY || choice->u.array.count < 3) {
                    continue;
                }
                if (!eval_cond(e, json_as_string(choice->u.array.items[1]))) {
                    continue;
                }
                if (e->choice_count >= MAX_CHOICES) {
                    break;
                }
                copy_str(e->choice_text[e->choice_count], sizeof(e->choice_text[0]),
                         json_as_string(choice->u.array.items[0]));
                e->choice_targets[e->choice_count] = (int)json_as_number(choice->u.array.items[2]);
                e->choice_count++;
            }
        }
        e->waiting = e->choice_count > 0;
        e->wait_auto = 0;
        e->wait_kind = e->choice_count > 0 ? WAIT_MENU : WAIT_NONE;
        c09_trace("menu: choices=%d pc=%d", e->choice_count, e->pc);
        return;
    }
    if (strcmp(kind, "if") == 0) {
        JsonValue *entries = op_item(op, 1);
        int i;
        int done = 0;
        if (entries && entries->type == JL_ARRAY) {
            size_t total = entries->u.array.count;
            for (i = 0; i < (int)total; i++) {
                JsonValue *entry = entries->u.array.items[i];
                if (!entry || entry->type != JL_ARRAY || entry->u.array.count < 2) {
                    continue;
                }
                if (eval_cond(e, json_as_string(entry->u.array.items[0]))) {
                    e->pc = (int)json_as_number(entry->u.array.items[1]);
                    done = 1;
                    break;
                }
            }
        }
        if (!done) {
            e->pc++;
        }
        return;
    }
    if (strcmp(kind, "hud") == 0) {
        e->hud = op_int(op, 1);
        e->pc++;
        return;
    }
    if (strcmp(kind, "ending_seen") == 0) {
        mark_seen(e, op_str(op, 1));
        e->pc++;
        return;
    }
    if (strcmp(kind, "ending") == 0) {
        begin_ending(e, op_str(op, 1));
        e->pc++;
        return;
    }
    e->pc++;
}

static void run_ops(Engine *e) {
    int guard = 0;
    while (e->mode == MODE_GAME && !e->waiting && !e->splash_active && e->running && guard++ < 20000) {
        execute_op(e);
    }
}

/* ------------------------------------------------------------------------- */
/* input actions                                                             */
/* ------------------------------------------------------------------------- */

enum {
    ACT_NONE = 0,
    ACT_CONFIRM,
    ACT_BACK,
    ACT_UP,
    ACT_DOWN,
    ACT_LEFT,
    ACT_RIGHT,
    ACT_PAUSE,
    ACT_ENDINGS
};

static int wrap_index(int value, int count) {
    if (count <= 0) {
        return 0;
    }
    value %= count;
    if (value < 0) {
        value += count;
    }
    return value;
}

#define FILE_COLS 2
#define FILE_ROWS 2
#define FILE_PAGE_SLOTS (FILE_COLS * FILE_ROWS)
#define FILE_PAGES ((MAX_SLOTS + FILE_PAGE_SLOTS - 1) / FILE_PAGE_SLOTS)
#define FILE_PAGE_FOCUS_COUNT (FILE_PAGES + 2)

static int file_selected_slot(Engine *e) {
    return e->file_page * FILE_PAGE_SLOTS + e->load_index;
}

static void file_set_page(Engine *e, int page) {
    if (page < 0) {
        page = FILE_PAGES - 1;
    }
    if (page >= FILE_PAGES) {
        page = 0;
    }
    e->file_page = page;
    e->page_focus = 1 + page;
}

static void file_confirm(Engine *e) {
    int slot;
    int is_save = e->mode == MODE_SAVE;
    int return_mode = is_save ? e->save_return_mode : e->load_return_mode;

    if (e->file_area == 1) {
        if (e->page_focus == 0) {
            file_set_page(e, e->file_page - 1);
        } else if (e->page_focus >= FILE_PAGES + 1) {
            file_set_page(e, e->file_page + 1);
        } else {
            file_set_page(e, e->page_focus - 1);
            e->file_area = 0;
            e->load_index = 0;
        }
        play_chunk(e, "sfx", "audio/MainMenuPress.mp3", 0, 0);
        return;
    }

    slot = file_selected_slot(e);
    if (slot < 0 || slot >= MAX_SLOTS) {
        return;
    }
    if (is_save) {
        char message[64];
        int ok;
        play_chunk(e, "sfx", "audio/MainMenuPress.mp3", 0, 0);
        ok = save_game(e, slot);
        c09_trace("save: slot=%d ok=%d", slot, ok);
        if (ok) {
            snprintf(message, sizeof(message), "Saved to slot %02d", slot + 1);
            set_toast(e, message);
        } else {
            set_toast(e, "Could not write the save file");
        }
        return;
    }
    if (!slot_exists(e, slot)) {
        set_toast(e, "That slot is empty");
        return;
    }
    play_chunk(e, "sfx", "audio/MainMenuPress.mp3", 0, 0);
    if (load_game(e, slot)) {
        c09_trace("load: slot=%d ok=1", slot);
        e->mode = return_mode;
        resume_game(e);
    } else {
        c09_trace("load: slot=%d ok=0", slot);
        set_toast(e, "Could not load that save");
    }
}

static void on_confirm(Engine *e) {
    if (e->mode == MODE_MENU) {
        play_chunk(e, "sfx", "audio/MainMenuPress.mp3", 0, 0);
        if (e->main_index == 0) {
            start_game(e);
        } else if (e->main_index == 1) {
            int slot = latest_slot(e);
            enter_load(e, MODE_MENU);
            if (slot >= 0) {
                e->file_page = slot / FILE_PAGE_SLOTS;
                e->load_index = slot % FILE_PAGE_SLOTS;
                e->page_focus = 1 + e->file_page;
            }
        } else if (e->main_index == 2) {
            enter_options(e, MODE_MENU);
        } else if (e->main_index == 3) {
            e->mode = MODE_ABOUT;
        } else if (e->main_index == 4) {
            e->running = 0;
        } else if (e->main_index == 5) {
            enter_endings(e);
        }
        return;
    }
    if (e->mode == MODE_ENDINGS) {
        play_chunk(e, "sfx", "audio/Phone_button.mp3", 0, 0);
        if (e->endings_detail) {
            e->endings_detail = 0;
        } else if (e->seen_count > 0) {
            e->endings_detail = 1;
            e->endings_scroll = 0;
        }
        return;
    }
    if (e->mode == MODE_GAME) {
        if (e->ending_active) {
            e->ending_active = 0;
            e->waiting = 0;
            e->wait_auto = 0;
            e->wait_kind = WAIT_NONE;
            e->line_shown = 0;
            return;
        }
        if (e->wait_kind == WAIT_MENU && e->choice_count > 0) {
            select_choice(e);
            return;
        }
        if (e->waiting) {
            e->waiting = 0;
            e->wait_auto = 0;
            e->wait_kind = WAIT_NONE;
            stop_voice(e);
        }
        return;
    }
    if (e->mode == MODE_PAUSE) {
        play_chunk(e, "sfx", "audio/MainMenuPress.mp3", 0, 0);
        if (e->pause_index == 0) {
            resume_game(e);
        } else if (e->pause_index == 1) {
            enter_save(e, MODE_PAUSE);
        } else if (e->pause_index == 2) {
            enter_load(e, MODE_PAUSE);
        } else if (e->pause_index == 3) {
            enter_options(e, MODE_PAUSE);
        } else if (e->pause_index == 4) {
            enter_main_menu(e);
        } else {
            e->running = 0;
        }
        return;
    }
    if (e->mode == MODE_SAVE || e->mode == MODE_LOAD) {
        file_confirm(e);
        return;
    }
    if (e->mode == MODE_OPTIONS) {
        if (e->options_index == 2) {
            e->mode = e->options_return_mode;
        }
        return;
    }
    if (e->mode == MODE_ABOUT) {
        e->mode = MODE_MENU;
        return;
    }
}

static void on_back(Engine *e) {
    if (e->mode == MODE_GAME) {
        if (!e->ending_active && e->wait_kind != WAIT_MENU) {
            history_back(e);
        }
    } else if (e->mode == MODE_PAUSE) {
        resume_game(e);
    } else if (e->mode == MODE_ENDINGS) {
        play_chunk(e, "sfx", "audio/Phone_close.mp3", 0, 0);
        if (e->endings_detail) {
            e->endings_detail = 0;
        } else {
            e->mode = MODE_MENU;
        }
    } else if (e->mode == MODE_SAVE) {
        e->mode = e->save_return_mode;
    } else if (e->mode == MODE_LOAD) {
        e->mode = e->load_return_mode;
    } else if (e->mode == MODE_OPTIONS) {
        e->mode = e->options_return_mode;
    } else if (e->mode == MODE_ABOUT) {
        e->mode = MODE_MENU;
    }
}

static void on_nav(Engine *e, int delta) {
    if (e->mode == MODE_ENDINGS) {
        if (e->endings_detail) {
            e->endings_scroll += delta * 40;
            if (e->endings_scroll < 0) {
                e->endings_scroll = 0;
            }
            if (e->endings_scroll > 600) {
                e->endings_scroll = 600;
            }
        } else if (e->seen_count > 0) {
            e->endings_index = wrap_index(e->endings_index + delta, e->seen_count);
            play_chunk(e, "sfx", "audio/Phone_button.mp3", 0, 0);
        }
        return;
    }
    if (e->mode == MODE_MENU) {
        int next = e->main_index + delta;
        if (next < 0) {
            next = 0;
        }
        if (next > 5) {
            next = 5;
        }
        if (next != e->main_index) {
            e->main_index = next;
            play_chunk(e, "sfx", "audio/MainMenuRollover.mp3", 0, 0);
        }
    } else if (e->mode == MODE_GAME) {
        if (e->wait_kind == WAIT_MENU && e->choice_count > 0) {
            e->choice_index = wrap_index(e->choice_index + delta, e->choice_count);
        }
    } else if (e->mode == MODE_PAUSE) {
        e->pause_index = wrap_index(e->pause_index + delta, 6);
        play_chunk(e, "sfx", "audio/MainMenuRollover.mp3", 0, 0);
    } else if (e->mode == MODE_SAVE || e->mode == MODE_LOAD) {
        play_chunk(e, "sfx", "audio/MainMenuRollover.mp3", 0, 0);
        if (e->file_area == 0) {
            if (delta > 0 && e->load_index >= FILE_COLS) {
                e->file_area = 1;
                e->page_focus = 1 + e->file_page;
            } else if (delta > 0) {
                e->load_index = e->load_index % FILE_COLS + FILE_COLS;
            } else if (delta < 0 && e->load_index >= FILE_COLS) {
                e->load_index -= FILE_COLS;
            }
        } else {
            if (delta < 0) {
                e->file_area = 0;
                e->load_index = FILE_COLS + e->load_index % FILE_COLS;
            }
        }
    } else if (e->mode == MODE_OPTIONS) {
        e->options_index = wrap_index(e->options_index + delta, 3);
        play_chunk(e, "sfx", "audio/MainMenuRollover.mp3", 0, 0);
    }
}

static void on_adjust(Engine *e, int delta) {
    if (e->mode == MODE_SAVE || e->mode == MODE_LOAD) {
        if (e->file_area == 0) {
            int column = e->load_index % FILE_COLS;
            column += delta;
            if (column < 0) {
                file_set_page(e, e->file_page - 1);
                column = FILE_COLS - 1;
            } else if (column >= FILE_COLS) {
                file_set_page(e, e->file_page + 1);
                column = 0;
            }
            e->load_index = (e->load_index / FILE_COLS) * FILE_COLS + column;
            if (e->load_index < 0) {
                e->load_index = 0;
            }
        } else {
            e->page_focus = wrap_index(e->page_focus + delta, FILE_PAGE_FOCUS_COUNT);
            if (e->page_focus >= 1 && e->page_focus <= FILE_PAGES) {
                e->file_page = e->page_focus - 1;
            }
        }
        play_chunk(e, "sfx", "audio/MainMenuRollover.mp3", 0, 0);
        return;
    }
    if (e->mode != MODE_OPTIONS) {
        return;
    }
    if (e->options_index == 0) {
        e->opt_music += delta;
        if (e->opt_music < 0) {
            e->opt_music = 0;
        }
        if (e->opt_music > 10) {
            e->opt_music = 10;
        }
        apply_volumes(e);
    } else if (e->options_index == 1) {
        e->opt_voice += delta;
        if (e->opt_voice < 0) {
            e->opt_voice = 0;
        }
        if (e->opt_voice > 10) {
            e->opt_voice = 10;
        }
        apply_volumes(e);
    }
}

static void handle_action(Engine *e, int action) {
    c09_trace("act: %d mode=%d pc=%d wait=%d line=%s", action, (int)e->mode, e->pc, e->waiting,
              e->line);
    if (e->newmsg_active && e->mode == MODE_MENU) {
        if (SDL_GetTicks() >= e->newmsg_skip_at) {
            e->newmsg_active = 0;
        }
        return;
    }
    switch (action) {
    case ACT_CONFIRM:
        on_confirm(e);
        break;
    case ACT_BACK:
        on_back(e);
        break;
    case ACT_UP:
        on_nav(e, -1);
        break;
    case ACT_DOWN:
        on_nav(e, 1);
        break;
    case ACT_LEFT:
        on_adjust(e, -1);
        break;
    case ACT_RIGHT:
        on_adjust(e, 1);
        break;
    case ACT_PAUSE:
        if (e->mode == MODE_GAME && !e->ending_active) {
            enter_pause(e);
        } else if (e->mode == MODE_PAUSE) {
            resume_game(e);
        } else if (e->mode != MODE_MENU) {
            on_back(e);
        }
        break;
    case ACT_ENDINGS:
        if (e->mode == MODE_MENU) {
            enter_endings(e);
        } else if (e->mode == MODE_ENDINGS) {
            on_back(e);
        }
        break;
    default:
        break;
    }
}

static int key_action(SDL_Keycode key) {
    switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
        return ACT_CONFIRM;
    case SDLK_ESCAPE:
        return ACT_BACK;
    case SDLK_UP:
    case SDLK_KP_8:
        return ACT_UP;
    case SDLK_DOWN:
    case SDLK_KP_2:
        return ACT_DOWN;
    case SDLK_LEFT:
    case SDLK_KP_4:
        return ACT_LEFT;
    case SDLK_RIGHT:
    case SDLK_KP_6:
        return ACT_RIGHT;
    case SDLK_TAB:
        return ACT_PAUSE;
    case SDLK_e:
        return ACT_ENDINGS;
    default:
        return ACT_NONE;
    }
}

static int button_action(Uint8 button) {
    switch (button) {
#ifdef __SWITCH__
    /* Nintendo layout: physical A (east) confirms, B (south) goes back. SDL
     * maps those to BUTTON_B and BUTTON_A respectively, so swap them here. */
    case SDL_CONTROLLER_BUTTON_A:
        return ACT_BACK;
    case SDL_CONTROLLER_BUTTON_B:
        return ACT_CONFIRM;
#else
    case SDL_CONTROLLER_BUTTON_A:
        return ACT_CONFIRM;
    case SDL_CONTROLLER_BUTTON_B:
        return ACT_BACK;
#endif
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        return ACT_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return ACT_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return ACT_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return ACT_RIGHT;
    case SDL_CONTROLLER_BUTTON_START:
    case SDL_CONTROLLER_BUTTON_BACK:
        return ACT_PAUSE;
    case SDL_CONTROLLER_BUTTON_Y:
        return ACT_ENDINGS;
    default:
        return ACT_NONE;
    }
}

static void open_controller(Engine *e, int device_index) {
    if (e->pad || !SDL_IsGameController(device_index)) {
        return;
    }
    e->pad = SDL_GameControllerOpen(device_index);
}

/* Left stick navigation with edge detection, so holding it moves one step at
 * a time exactly like the D-pad. */
static void handle_stick(Engine *e, int axis, int value) {
    static int stick_x = 0;
    static int stick_y = 0;
    static int stick_last = 0;
    int dx;
    int dy;
    int dir;
    if (axis == 0) {
        stick_x = value;
    } else if (axis == 1) {
        stick_y = value;
    } else {
        return;
    }
    dx = (stick_x > 16000) - (stick_x < -16000);
    dy = (stick_y > 16000) - (stick_y < -16000);
    dir = 0;
    if (dx || dy) {
        if (abs(stick_x) >= abs(stick_y)) {
            dir = dx < 0 ? ACT_LEFT : ACT_RIGHT;
        } else {
            dir = dy < 0 ? ACT_UP : ACT_DOWN;
        }
    }
    if (dir != stick_last) {
        stick_last = dir;
        if (dir) {
            handle_action(e, dir);
        }
    }
}

static void dump_state(Engine *e);
static void finish_splash(Engine *e);

static void poll_events(Engine *e) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            e->running = 0;
            continue;
        }
        if (e->splash_active) {
            if (event.type == SDL_KEYDOWN || event.type == SDL_CONTROLLERBUTTONDOWN ||
                event.type == SDL_MOUSEBUTTONDOWN) {
                /* Ending videos play to the end; the intro and logo may be skipped. */
                if (e->splash_mode != 2 && SDL_GetTicks() >= e->splash_skip_at) {
                    finish_splash(e);
                }
                continue;
            }
            if (event.type != SDL_CONTROLLERDEVICEADDED && event.type != SDL_CONTROLLERDEVICEREMOVED) {
                continue;
            }
        }
        if (event.type == SDL_KEYDOWN && !event.key.repeat) {
            if (event.key.keysym.sym == SDLK_F11) {
                Uint32 flags = SDL_GetWindowFlags(e->window);
                SDL_SetWindowFullscreen(e->window, (flags & SDL_WINDOW_FULLSCREEN) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            } else if (event.key.keysym.sym == SDLK_F12) {
                printf("class09: screenshot requested\n");
                e->shot_requested = 1;
            } else if (event.key.keysym.sym == SDLK_F9) {
                dump_state(e);
            } else {
                handle_action(e, key_action(event.key.keysym.sym));
            }
        } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
            handle_action(e, button_action(event.cbutton.button));
#ifdef __SWITCH__
        } else if (event.type == SDL_JOYBUTTONDOWN) {
            /* Fallback when SDL has no GameController mapping for this pad. */
            if (!e->pad) {
                int action = ACT_NONE;
                switch (event.jbutton.button) {
                case 0:
                    action = ACT_BACK;
                    break;
                case 1:
                    action = ACT_CONFIRM;
                    break;
                case 3:
                    action = ACT_ENDINGS;
                    break;
                case 10:
                case 11:
                    action = ACT_PAUSE;
                    break;
                case 12:
                    action = ACT_LEFT;
                    break;
                case 13:
                    action = ACT_UP;
                    break;
                case 14:
                    action = ACT_RIGHT;
                    break;
                case 15:
                    action = ACT_DOWN;
                    break;
                default:
                    break;
                }
                if (action != ACT_NONE) {
                    handle_action(e, action);
                }
            }
        } else if (event.type == SDL_JOYAXISMOTION) {
            handle_stick(e, event.jaxis.axis, event.jaxis.value);
#endif
        } else if (event.type == SDL_CONTROLLERAXISMOTION) {
            if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ||
                event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                handle_stick(e, (int)event.caxis.axis, event.caxis.value);
            }
        } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
            open_controller(e, event.cdevice.which);
        } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (e->pad) {
                SDL_Joystick *joystick = SDL_GameControllerGetJoystick(e->pad);
                SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
                if (id == event.cdevice.which) {
                    SDL_GameControllerClose(e->pad);
                    e->pad = NULL;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* drawing primitives                                                        */
/* ------------------------------------------------------------------------- */

static void fill_rect(Engine *e, int x, int y, int w, int h, Uint32 rgb, int alpha) {
    SDL_Rect rect;
    SDL_Color color = color_of(rgb);
    if (!e->renderer) {
        return;
    }
    if (alpha < 0) {
        alpha = 0;
    }
    if (alpha > 255) {
        alpha = 255;
    }
    SDL_SetRenderDrawBlendMode(e->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(e->renderer, color.r, color.g, color.b, (Uint8)alpha);
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    SDL_RenderFillRect(e->renderer, &rect);
}

static int draw_image_file(Engine *e, const char *full, int x, int y, int w, int h, int alpha, int flip) {
    int tw = 0;
    int th = 0;
    SDL_Texture *texture = get_texture_at(e, full, &tw, &th);
    SDL_Rect dst;
    if (!texture) {
        return 0;
    }
    if (w <= 0) {
        w = tw;
    }
    if (h <= 0) {
        h = th;
    }
    if (alpha < 0) {
        alpha = 0;
    }
    if (alpha > 255) {
        alpha = 255;
    }
    SDL_SetTextureAlphaMod(texture, (Uint8)alpha);
    dst.x = x;
    dst.y = y;
    dst.w = w;
    dst.h = h;
    if (flip) {
        SDL_RenderCopyEx(e->renderer, texture, NULL, &dst, 0, NULL, SDL_FLIP_HORIZONTAL);
    } else {
        SDL_RenderCopy(e->renderer, texture, NULL, &dst);
    }
    SDL_SetTextureAlphaMod(texture, 255);
    return 1;
}

static int draw_image(Engine *e, const char *rel, int x, int y, int w, int h, int alpha, int flip) {
    char full[MAX_PATH];
    if (!e || !rel || !rel[0]) {
        return 0;
    }
    join_path(full, sizeof(full), e->game_dir, rel);
    return draw_image_file(e, full, x, y, w, h, alpha, flip);
}

static void item_geometry(Engine *e, LayerItem *item, int *x, int *y, int *w, int *h, int *alpha, int *flip) {
    int i;
    *x = item->x;
    *y = item->y;
    *w = item->w;
    *h = item->h;
    *alpha = item->alpha;
    *flip = item->flip;
    if (item->anim_count < 2) {
        return;
    }
    {
        Uint32 clock = item->anim_paused ? e->pause_started : SDL_GetTicks();
        Uint32 elapsed = clock - item->anim_start;
        int last = item->anim_count - 1;
        if ((int)elapsed >= item->anim[last][0]) {
            return;
        }
        for (i = 0; i < last; i++) {
            int t0 = item->anim[i][0];
            int t1 = item->anim[i + 1][0];
            int k;
            int ratio;
            if ((int)elapsed > t1) {
                continue;
            }
            if ((int)elapsed <= t0) {
                for (k = 0; k < 4; k++) {
                    switch (k) {
                    case 0:
                        *x = item->anim[i][1];
                        break;
                    case 1:
                        *y = item->anim[i][2];
                        break;
                    case 2:
                        *w = item->anim[i][3];
                        break;
                    default:
                        *h = item->anim[i][4];
                        break;
                    }
                }
                *alpha = item->anim[i][5];
                *flip = item->anim[i][6];
                return;
            }
            ratio = t1 > t0 ? (int)((elapsed - t0) * 1000 / (t1 - t0)) : 1000;
            *x = item->anim[i][1] + (item->anim[i + 1][1] - item->anim[i][1]) * ratio / 1000;
            *y = item->anim[i][2] + (item->anim[i + 1][2] - item->anim[i][2]) * ratio / 1000;
            *w = item->anim[i][3] + (item->anim[i + 1][3] - item->anim[i][3]) * ratio / 1000;
            *h = item->anim[i][4] + (item->anim[i + 1][4] - item->anim[i][4]) * ratio / 1000;
            *alpha = item->anim[i][5] + (item->anim[i + 1][5] - item->anim[i][5]) * ratio / 1000;
            *flip = ratio >= 1000 ? item->anim[i + 1][6] : item->anim[i][6];
            return;
        }
    }
}

static void draw_item(Engine *e, LayerItem *item) {
    int x;
    int y;
    int w;
    int h;
    int alpha;
    int flip;
    if (!item->used) {
        return;
    }
    item_geometry(e, item, &x, &y, &w, &h, &alpha, &flip);
    if (strcmp(item->kind, "image") == 0) {
        draw_image(e, item->rel, x, y, w, h, alpha, flip);
    } else if (strcmp(item->kind, "solid") == 0) {
        int r = 0;
        int g = 0;
        int b = 0;
        int a = 255;
        if (item->arg[0]) {
            sscanf(item->arg, "%d , %d , %d , %d", &r, &g, &b, &a);
        }
        fill_rect(e, x, y, w, h, (Uint32)((r & 0xFF) << 16 | (g & 0xFF) << 8 | (b & 0xFF)), a * alpha / 255);
    }
}

/* ------------------------------------------------------------------------- */
/* in-game drawing                                                           */
/* ------------------------------------------------------------------------- */

static void draw_dialogue(Engine *e) {
    int textbox_h = 0;
    int drop = 0;
    if (!e->hud) {
        return;
    }
    if (!e->line[0] && !e->speaker[0]) {
        return;
    }
    if (e->gui_textbox[0]) {
        int tw = 0;
        int th = 0;
        get_texture(e, e->gui_textbox, &tw, &th);
        if (th > 0) {
            textbox_h = th;
            draw_image(e, e->gui_textbox, 0, SCREEN_H - th, SCREEN_W, th, 255, 0);
        }
    }
    if (textbox_h == 0) {
        textbox_h = 278;
        fill_rect(e, 0, SCREEN_H - 278, SCREEN_W, 278, 0x000000, 170);
    }
    /* The shipped textbox art is 229 px tall (top edge at y=851) while the
     * name line is positioned at y=806 for the 278 px GUI layout.  Drop the
     * name and dialogue together so they sit on the visible paper. */
    drop = (SCREEN_H - textbox_h + 2) - e->gui_name_y;
    if (drop < 0) {
        drop = 0;
    }
    if (drop > 60) {
        drop = 60;
    }
    if (e->speaker[0]) {
        draw_text(e, FONT_NAME, e->speaker, e->gui_name_color, 0, e->gui_name_x, e->gui_name_y + drop, 0, 0);
    }
    if (e->line[0]) {
        draw_text(e, FONT_DIALOGUE, e->line, e->gui_dialogue_color, e->gui_dialogue_w, e->gui_dialogue_x,
                  e->gui_dialogue_y + drop, 0, e->gui_dialogue_spacing);
    }
}

/* Width of the widest '\n'-separated line, so designed two-line choices keep
 * their breaks instead of being re-wrapped. */
static int widest_line(TTF_Font *font, const char *text) {
    char line[512];
    int widest = 0;
    const char *p = text;
    if (!font || !text) {
        return 0;
    }
    for (;;) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        int w = 0;
        int h = 0;
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        memcpy(line, p, len);
        line[len] = 0;
        if (TTF_SizeUTF8(font, line, &w, &h) == 0 && w > widest) {
            widest = w;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return widest;
}

/* Largest choice font whose widest line fits the button. */
static int fit_choice_font(Engine *e, const char *text, int max_width) {
    static const int ids[] = {FONT_CHOICE, FONT_CHOICE_SMALL, FONT_CHOICE_TINY, FONT_CHOICE_MINI};
    int i;
    for (i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); i++) {
        TTF_Font *font = ensure_font(e, ids[i]);
        if (font && widest_line(font, text) <= max_width) {
            return ids[i];
        }
    }
    return ids[(int)(sizeof(ids) / sizeof(ids[0])) - 1];
}

static void draw_choices(Engine *e) {
    int i;
    int y = e->gui_choice_y;
    if (e->gui_choice_window[0]) {
        draw_image(e, e->gui_choice_window, 0, 0, SCREEN_W, SCREEN_H, 255, 0);
    } else {
        fill_rect(e, 0, 0, SCREEN_W, SCREEN_H, 0x000000, 90);
    }
    for (i = 0; i < e->choice_count; i++) {
        int row_h = e->gui_choice_h;
        Uint32 color = (i == e->choice_index) ? e->gui_choice_hover_color : e->gui_choice_idle_color;
        const char *background = (i == e->choice_index) ? e->gui_choice_hover : e->gui_choice_idle;
        TextBlock *block;
        int height;
        int text_font;
        /* The shipped button art is 1185 px wide with transparent margins; the
         * visible strip is ~766 px, so shrink text to that width. */
        const int wrap = 720;
        text_font = fit_choice_font(e, e->choice_text[i], wrap);
        block = text_block(e, text_font, e->choice_text[i], color, wrap);
        height = block_height(e, block, 0);
        if (height + 12 > row_h) {
            row_h = height + 12;
        }
        if (background[0]) {
            draw_image(e, background, e->gui_choice_x, y, e->gui_choice_w, row_h, 255, 0);
        }
        draw_block(e, block, e->gui_choice_x + e->gui_choice_w / 2, y + (row_h - height) / 2, 1, 0);
        y += row_h + e->gui_choice_spacing;
    }
}

static void draw_ending(Engine *e) {
    int fonts[3] = {FONT_BODY, FONT_SMALL, FONT_BODY_TINY};
    int k;
    fill_rect(e, 0, 0, SCREEN_W, SCREEN_H, 0x000000, 255);
    draw_text(e, FONT_TITLE, e->ending_title, 0x279BE8, 0, SCREEN_W / 2, 110, 1, 0);
    if (e->ending_body[0]) {
        for (k = 0; k < 3; k++) {
            TextBlock *block = text_block(e, fonts[k], e->ending_body, 0xEDEDED, 1400);
            int height = block_height(e, block, 4);
            if (height <= SCREEN_H - 270 - 80 || k == 2) {
                draw_block(e, block, 260, 270, 0, 4);
                break;
            }
        }
    }
    draw_text(e, FONT_SMALL, "Cross / Enter to continue", 0x888888, 0, SCREEN_W / 2, SCREEN_H - 50, 1, 0);
}

static void draw_layer(Engine *e, const char *layer) {
    int last = -1;
    for (;;) {
        LayerItem *best = NULL;
        int i;
        for (i = 0; i < MAX_ITEMS; i++) {
            LayerItem *item = &e->items[i];
            if (!item->used || strcmp(item->layer, layer) != 0 || item->order <= last) {
                continue;
            }
            if (!best || item->order < best->order) {
                best = item;
            }
        }
        if (!best) {
            break;
        }
        draw_item(e, best);
        last = best->order;
    }
}

static void draw_world(Engine *e) {
    draw_layer(e, "master");
    draw_percpop(e);
    draw_layer(e, "screens");
    if (e->ending_active) {
        draw_ending(e);
        return;
    }
    if (e->mode == MODE_GAME && e->wait_kind == WAIT_MENU && e->choice_count > 0) {
        draw_choices(e);
        return;
    }
    draw_dialogue(e);
    if (e->mode == MODE_GAME && !e->ending_active && e->wait_kind != WAIT_MENU) {
        draw_image(e, "gui/back_pills_vert.png", 156, 836, 110, 240, 255, 0);
        draw_image(e, "gui/pause_phone.png", 1515, 884, 350, 176, 255, 0);
    }
}

/* ------------------------------------------------------------------------- */
/* UI screens                                                                */
/* ------------------------------------------------------------------------- */

static void draw_rows(Engine *e, const char *const *labels, int count, int selected, int x, int y, int step,
                      int font, Uint32 idle, Uint32 hover, int align) {
    int i;
    for (i = 0; i < count; i++) {
        draw_text(e, font, labels[i], i == selected ? hover : idle, 0, x, y + i * step, align, 0);
    }
}

static const char *MAIN_BUTTONS[5] = {"NEWGAME", "CONTINUE", "OPTIONS", "ABOUT", "EXIT"};

static void draw_main_menu(Engine *e) {
    int i;
    char rel[MAX_PATH];
    if (e->gui_main_menu[0]) {
        draw_image(e, e->gui_main_menu, 0, 0, SCREEN_W, SCREEN_H, 255, 0);
    } else {
        fill_rect(e, 0, 0, SCREEN_W, SCREEN_H, 0x1A1A22, 255);
    }
    if (e->gui_logo[0]) {
        draw_image(e, e->gui_logo, e->gui_logo_x, e->gui_logo_y, e->gui_logo_w, e->gui_logo_h, 255, 0);
    }
    for (i = 0; i < 5; i++) {
        snprintf(rel, sizeof(rel), "images/%s.png", MAIN_BUTTONS[i]);
        draw_image(e, rel, 0, 0, SCREEN_W, SCREEN_H, 255, 0);
    }
    if (e->main_index < 5) {
        snprintf(rel, sizeof(rel), "images/%s_selected.png", MAIN_BUTTONS[e->main_index]);
        draw_image(e, rel, 0, 0, SCREEN_W, SCREEN_H, 255, 0);
    }
    draw_image(e, "gui/ending_tracker/text_access.png", 1569, 864, 225, 120, 255, 0);
    if (e->main_index == 5) {
        fill_rect(e, 1558, 853, 248, 4, 0x66C1E0, 255);
        fill_rect(e, 1558, 991, 248, 4, 0x66C1E0, 255);
        fill_rect(e, 1558, 853, 4, 142, 0x66C1E0, 255);
        fill_rect(e, 1802, 853, 4, 142, 0x66C1E0, 255);
    }
    draw_text(e, FONT_SMALL,
              e->main_index == 5
                  ? "Cross: endings   D-pad: move   Triangle: endings"
                  : "Cross: select   D-pad: move   Triangle: endings",
              0x9A9A9A, 0, 30, SCREEN_H - 40, 0, 0);
}

static void draw_new_message(Engine *e) {
    char rel[MAX_PATH];
    Uint32 now;
    if (!e->newmsg_active) {
        return;
    }
    now = SDL_GetTicks();
    if (now >= e->newmsg_next) {
        e->newmsg_frame++;
        e->newmsg_next = now + 50;
        if (e->newmsg_frame >= 59) {
            e->newmsg_active = 0;
            return;
        }
    }
    snprintf(rel, sizeof(rel), "NewMessageSequence/NewMessage_%04d.png", e->newmsg_frame + 1);
    draw_image(e, rel, 0, 0, SCREEN_W, SCREEN_H, 255, 0);
}

static const char *PAUSE_LABELS[6] = {"Resume", "Save Game", "Load Game", "Options", "Main Menu", "Quit"};

static void draw_pause(Engine *e) {
    fill_rect(e, 0, 0, SCREEN_W, SCREEN_H, 0x000000, 190);
    draw_text(e, FONT_TITLE, "PAUSED", 0xFFFFFF, 0, SCREEN_W / 2, 100, 1, 0);
    draw_rows(e, PAUSE_LABELS, 6, e->pause_index, SCREEN_W / 2, 270, 86, FONT_MENU, 0x888888, 0x66C1E0, 1);
    draw_text(e, FONT_SMALL, "Cross: select   Circle: resume", 0x777777, 0, SCREEN_W / 2, SCREEN_H - 70, 1, 0);
}

#define SLOT_CARD_W 414
#define SLOT_CARD_H 309
#define SLOT_CARD_GAP 15
#define SLOT_THUMB_W 384
#define SLOT_THUMB_H 216
#define SLOT_GRID_W (FILE_COLS * SLOT_CARD_W + (FILE_COLS - 1) * SLOT_CARD_GAP)
#define SLOT_GRID_H (FILE_ROWS * SLOT_CARD_H + (FILE_ROWS - 1) * SLOT_CARD_GAP)
#define SLOT_PAD 15

static void truncate_text(char *text, size_t size, int max_chars) {
    size_t len = strlen(text);
    if ((int)len <= max_chars || size < 5) {
        return;
    }
    if ((size_t)max_chars + 1 > size) {
        max_chars = (int)size - 1;
    }
    text[max_chars - 1] = '.';
    text[max_chars - 2] = '.';
    text[max_chars - 3] = '.';
    text[max_chars] = 0;
}

static void draw_slots(Engine *e) {
    int is_save = e->mode == MODE_SAVE;
    int grid_x = (SCREEN_W - SLOT_GRID_W) / 2;
    int grid_y = (SCREEN_H - SLOT_GRID_H) * 2 / 10;
    int i;
    int page_button_w = 50;
    int page_button_h = 45;
    int arrow_size = 32;
    int page_total = arrow_size * 2 + FILE_PAGES * page_button_w;
    int page_x = (SCREEN_W - page_total) / 2;
    int page_y = (SCREEN_H - page_button_h) * 75 / 100;
    char text[512];

    if (!draw_image(e, "gui/nvl.png", 0, 0, SCREEN_W, SCREEN_H, 255, 0)) {
        fill_rect(e, 0, 0, SCREEN_W, SCREEN_H, 0x000000, 205);
    }

    snprintf(text, sizeof(text), "Page %d", e->file_page + 1);
    draw_text(e, FONT_SMALL, text, 0xFFFFFF, 0, SCREEN_W / 2, 8, 1, 0);

    for (i = 0; i < FILE_PAGE_SLOTS; i++) {
        int slot = e->file_page * FILE_PAGE_SLOTS + i;
        int col = i % FILE_COLS;
        int row = i / FILE_COLS;
        int x = grid_x + col * (SLOT_CARD_W + SLOT_CARD_GAP);
        int y = grid_y + row * (SLOT_CARD_H + SLOT_CARD_GAP);
        int selected = e->file_area == 0 && e->load_index == i;
        int filled = slot_exists(e, slot);
        Uint32 color = selected ? 0x66C1E0 : 0xAAAAAA;
        char thumb[SAVE_PATH_MAX];

        draw_image(e, selected ? "gui/button/slot_hover_background.png" : "gui/button/slot_idle_background.png", x, y,
                   SLOT_CARD_W, SLOT_CARD_H, 255, 0);

        thumbnail_path(e, slot, thumb, sizeof(thumb));
        if (slot_thumbnail_exists(e, slot)) {
            draw_image_file(e, thumb, x + SLOT_PAD, y + SLOT_PAD, SLOT_THUMB_W, SLOT_THUMB_H, 255, 0);
        }

        if (filled) {
            format_slot_time(slot_time(e, slot), text, sizeof(text));
        } else {
            copy_str(text, sizeof(text), "empty slot");
        }
        draw_text(e, FONT_SLOT, text, color, 0, x + SLOT_CARD_W / 2, y + SLOT_PAD + SLOT_THUMB_H, 1, 0);

        if (filled) {
            preview_slot(e, slot, text, sizeof(text));
            truncate_text(text, sizeof(text), 38);
            draw_text(e, FONT_SLOT, text, color, 0, x + SLOT_CARD_W / 2, y + SLOT_PAD + SLOT_THUMB_H + 24, 1, 0);
        }
    }

    draw_image(e, "gui/arrowleft.png", page_x, page_y + (page_button_h - arrow_size) / 2, arrow_size, arrow_size,
               (e->file_area == 1 && e->page_focus == 0) ? 255 : 140, 0);
    for (i = 0; i < FILE_PAGES; i++) {
        int bx = page_x + arrow_size + i * page_button_w;
        Uint32 color = 0x888888;
        if (e->file_area == 1 && e->page_focus == i + 1) {
            color = 0x66C1E0;
        } else if (i == e->file_page) {
            color = 0xFFFFFF;
        }
        snprintf(text, sizeof(text), "%d", i + 1);
        draw_text(e, FONT_SMALL, text, color, 0, bx + page_button_w / 2, page_y + 6, 1, 0);
    }
    draw_image(e, "gui/arrowright.png", page_x + arrow_size + FILE_PAGES * page_button_w,
               page_y + (page_button_h - arrow_size) / 2, arrow_size, arrow_size,
               (e->file_area == 1 && e->page_focus >= FILE_PAGES + 1) ? 255 : 140, 0);

    draw_text(e, FONT_SLOT, is_save ? "Cross: save    Circle: back" : "Cross: load    Circle: back", 0x777777, 0,
              SCREEN_W / 2, SCREEN_H - 34, 1, 0);
}

static void draw_options(Engine *e) {
    char label[128];
    fill_rect(e, 0, 0, SCREEN_W, SCREEN_H, 0x000000, 210);
    draw_text(e, FONT_TITLE, "OPTIONS", 0xFFFFFF, 0, SCREEN_W / 2, 130, 1, 0);

    snprintf(label, sizeof(label), "Music Volume      %d / 10", e->opt_music);
    draw_text(e, FONT_MENU, label, e->options_index == 0 ? 0x66C1E0 : 0x888888, 0, SCREEN_W / 2, 320, 1, 0);
    snprintf(label, sizeof(label), "Voice Volume      %d / 10", e->opt_voice);
    draw_text(e, FONT_MENU, label, e->options_index == 1 ? 0x66C1E0 : 0x888888, 0, SCREEN_W / 2, 420, 1, 0);
    draw_text(e, FONT_MENU, "Back", e->options_index == 2 ? 0x66C1E0 : 0x888888, 0, SCREEN_W / 2, 520, 1, 0);
    draw_text(e, FONT_SMALL, "Left / Right changes the selected value", 0x777777, 0, SCREEN_W / 2, 700, 1, 0);
}

static void draw_about(Engine *e) {
    char body[MAX_TEXT];
    int total = (e->endings && e->endings->type == JL_ARRAY) ? (int)e->endings->u.array.count : 15;
    fill_rect(e, 0, 0, SCREEN_W, SCREEN_H, 0x000000, 220);
    draw_text(e, FONT_TITLE, "ABOUT", 0xFFFFFF, 0, SCREEN_W / 2, 90, 1, 0);
    snprintf(body, sizeof(body),
             "Ported by CatsAndGats\n\n"
             "Class of '09 - unofficial PS5 homebrew runtime.\n\n"
             "This runtime is an unofficial fan project. Game assets remain the property of "
             "their owners and are read from the copy of Class of '09 installed with this app.\n\n"
             "Endings discovered: %d / %d\n\n"
             "Cross: advance / confirm\n"
             "Circle: step back one line\n"
             "Triangle: ending tracker (main menu)\n"
             "D-pad: navigate\n"
             "Options / Start: in-game menu\n\n"
             "Not affiliated with the developers, publishers, or Sony.",
             e->seen_count, total);
    draw_text(e, FONT_SMALL, body, 0xDDDDDD, 1300, 260, 190, 0, 8);
    draw_text(e, FONT_SMALL, "Cross to return", 0x777777, 0, SCREEN_W / 2, SCREEN_H - 60, 1, 0);
}

static JsonValue *seen_ending_at(Engine *e, int index) {
    int i;
    int found = 0;
    int total;
    if (!e->endings || e->endings->type != JL_ARRAY) {
        return NULL;
    }
    total = (int)e->endings->u.array.count;
    for (i = 0; i < total; i++) {
        JsonValue *item = e->endings->u.array.items[i];
        const char *id = obj_str(item, "id", NULL);
        if (id && ending_is_seen(e, id)) {
            if (found == index) {
                return item;
            }
            found++;
        }
    }
    return NULL;
}

static void draw_endings(Engine *e) {
    char text[64];
    char row[512];
    int i;
    int total = (e->endings && e->endings->type == JL_ARRAY) ? (int)e->endings->u.array.count : 15;
    time_t now;
    struct tm *clock_now;
    JsonValue *item;

    /* Show the whole Sidekick photo, and shrink the messaging UI into the
     * phone's display instead of zooming the photo to fill the screen. */
    draw_image(e, "gui/ending_tracker/ending_tracker_pda.png", 0, 0, SCREEN_W, SCREEN_H, 255, 0);
    draw_image(e, "gui/ending_tracker/base_screen.png", 460, 274, 989, 556, 255, 0);

    snprintf(text, sizeof(text), "%d/%d", e->seen_count, total);
    draw_text(e, FONT_TRACKER, text, 0x2370BE, 0, 670, 342, 0, 0);
    draw_text(e, FONT_TRACKER, text, 0xFFFFFF, 0, 668, 340, 0, 0);

    now = time(NULL);
    clock_now = localtime(&now);
    if (clock_now) {
        int hour = clock_now->tm_hour % 12;
        if (hour == 0) {
            hour = 12;
        }
        snprintf(text, sizeof(text), "%02d:%02d", hour, clock_now->tm_min);
        draw_text(e, FONT_TRACKER, text, 0x0000CC, 0, 1098, 305, 0, 0);
    }

    if (e->endings_detail) {
        item = seen_ending_at(e, e->endings_index);
        if (item) {
            draw_text(e, FONT_TRACKER, obj_str(item, "text", ""), 0x000000, 633, 599,
                      403 - (int)(e->endings_scroll * 0.515), 0, 4);
        }
        draw_text(e, FONT_TRACKER, "D-pad: scroll   Cross / Circle: back", 0x2370BE, 0, 604, 758, 0, 0);
        return;
    }

    {
        const int rows = 10;
        int last;
        if (e->endings_index < e->endings_top) {
            e->endings_top = e->endings_index;
        }
        if (e->endings_index >= e->endings_top + rows) {
            e->endings_top = e->endings_index - rows + 1;
        }
        if (e->endings_top > e->seen_count - rows && e->seen_count > rows) {
            e->endings_top = e->seen_count - rows;
        }
        if (e->endings_top < 0) {
            e->endings_top = 0;
        }
        last = e->endings_top + rows;
        if (last > e->seen_count) {
            last = e->seen_count;
        }
        for (i = e->endings_top; i < last; i++) {
            int y = 392 + (i - e->endings_top) * 36;
            item = seen_ending_at(e, i);
            if (!item) {
                continue;
            }
            if (i == e->endings_index) {
                draw_image(e, "gui/ending_tracker/highlight_bar.png", 593, y, 663, 36, 255, 0);
            }
            snprintf(row, sizeof(row), "%s", obj_str(item, "title", "UNKNOWN"));
            {
                int row_font = FONT_TRACKER;
                TTF_Font *row_face = ensure_font(e, FONT_TRACKER);
                if (row_face && widest_line(row_face, row) > 650) {
                    row_font = FONT_TRACKER_SMALL;
                }
                draw_text(e, row_font, row, i == e->endings_index ? 0xFFFFFF : 0x000000, 0, 606, y + 5, 0, 0);
            }
        }
    }
    if (e->seen_count == 0) {
        draw_text(e, FONT_TRACKER, "No endings collected yet.", 0x000000, 0, 606, 392, 0, 0);
    }
    draw_text(e, FONT_TRACKER, "D-pad: choose/scroll   Cross: read   Circle: back", 0x2370BE, 0, 604, 758, 0, 0);
}

static void draw_toast(Engine *e) {
    if (!e->toast[0] || (Sint32)(e->toast_until - SDL_GetTicks()) <= 0) {
        return;
    }
    fill_rect(e, SCREEN_W / 2 - 350, 26, 700, 64, 0x000000, 200);
    draw_text(e, FONT_SMALL, e->toast, 0xFFFFFF, 0, SCREEN_W / 2, 40, 1, 0);
}

static void dump_state(Engine *e) {
    int i;
    printf("class09: mode=%d pc=%d waiting=%d wait_kind=%d wait_auto=%d hud=%d ending=%d\n", e->mode, e->pc,
            e->waiting, e->wait_kind, e->wait_auto, e->hud, e->ending_active);
    printf("class09: speaker=[%s] line=[%s]\n", e->speaker, e->line);
    printf("class09: voice=[%s] ambient=[%s] music=[%s]\n", e->voice, e->ambient_path, e->music_path);
    if (e->mode == MODE_SAVE || e->mode == MODE_LOAD) {
        printf("class09: slots page=%d/%d area=%d index=%d focus=%d slot=%d\n", e->file_page + 1, FILE_PAGES,
                e->file_area, e->load_index, e->page_focus, file_selected_slot(e));
    }
    if (e->ops && e->ops->type == JL_ARRAY && e->pc >= 0 && (size_t)e->pc < e->ops->u.array.count) {
        JsonValue *op = e->ops->u.array.items[e->pc];
        printf("class09: next=%s %s\n", op_str(op, 0), op_str(op, 1));
    }
    for (i = 0; i < MAX_ITEMS; i++) {
        LayerItem *item = &e->items[i];
        if (!item->used) {
            continue;
        }
        printf("class09: item %s/%s %s rel=[%s] arg=[%s] %d,%d %dx%d a=%d f=%d o=%d anim=%d\n", item->layer,
                item->tag, item->kind, item->rel, item->arg, item->x, item->y, item->w, item->h, item->alpha,
                item->flip, item->order, item->anim_count);
        if (item->anim_count >= 2) {
            int ax, ay, aw, ah, aa, af;
            item_geometry(e, item, &ax, &ay, &aw, &ah, &aa, &af);
            printf("class09:   anim now %d,%d %dx%d a=%d f=%d over %dms\n", ax, ay, aw, ah, aa, af,
                    item->anim[item->anim_count - 1][0]);
        }
    }
}

static void capture_frame(Engine *e) {
    SDL_Surface *surface;
    char path[SAVE_PATH_MAX];
    void *pixels;
    int out_w = SCREEN_W;
    int out_h = SCREEN_H;

    if (!e->renderer) {
        return;
    }
    printf("class09: capture page pc=%d mode=%d waiting=%d kind=%d hud=%d line=[%s]\n", e->pc, e->mode,
            e->waiting, e->wait_kind, e->hud, e->line);
    SDL_GetRendererOutputSize(e->renderer, &out_w, &out_h);
    if (out_w <= 0 || out_h <= 0) {
        out_w = SCREEN_W;
        out_h = SCREEN_H;
    }
    surface = SDL_CreateRGBSurfaceWithFormat(0, out_w, out_h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) {
        printf("class09: screenshot surface failed: %s\n", SDL_GetError());
        return;
    }
    pixels = surface->pixels;
    if (SDL_RenderReadPixels(e->renderer, NULL, SDL_PIXELFORMAT_ARGB8888, pixels, surface->pitch) != 0) {
        printf("class09: screenshot read failed: %s\n", SDL_GetError());
        SDL_FreeSurface(surface);
        return;
    }
    snprintf(path, sizeof(path), "%s/screenshot.bmp", e->save_dir);
    if (SDL_SaveBMP(surface, path) != 0) {
        printf("class09: screenshot write failed: %s\n", SDL_GetError());
    } else {
        printf("class09: screenshot saved: %s\n", path);
    }
    SDL_FreeSurface(surface);
}

static void capture_pause_thumbnail(Engine *e) {
    SDL_Surface *source;
    SDL_Surface *thumb;
    char path[SAVE_PATH_MAX];
    int out_w = SCREEN_W;
    int out_h = SCREEN_H;

    if (!e->renderer) {
        return;
    }
    SDL_GetRendererOutputSize(e->renderer, &out_w, &out_h);
    if (out_w <= 0 || out_h <= 0) {
        out_w = SCREEN_W;
        out_h = SCREEN_H;
    }
    source = SDL_CreateRGBSurfaceWithFormat(0, out_w, out_h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!source) {
        return;
    }
    if (SDL_RenderReadPixels(e->renderer, NULL, SDL_PIXELFORMAT_ARGB8888, source->pixels, source->pitch) != 0) {
        printf("class09: thumbnail read failed: %s\n", SDL_GetError());
        SDL_FreeSurface(source);
        return;
    }
    thumb = SDL_CreateRGBSurfaceWithFormat(0, SLOT_THUMB_W, SLOT_THUMB_H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!thumb) {
        SDL_FreeSurface(source);
        return;
    }
    SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
    SDL_BlitScaled(source, NULL, thumb, NULL);
    pause_thumbnail_path(e, path, sizeof(path));
    if (SDL_SaveBMP(thumb, path) != 0) {
        printf("class09: thumbnail write failed: %s\n", SDL_GetError());
    }
    SDL_FreeSurface(thumb);
    SDL_FreeSurface(source);
}

/* ------------------------------------------------------------------------- */
/* configuration                                                             */
/* ------------------------------------------------------------------------- */

static void load_gui(Engine *e) {
    JsonValue *gui = e->gui;
    JsonValue *dialogue;
    JsonValue *name;
    JsonValue *choice;
    JsonValue *menu;
    JsonValue *logo;
    int choice_w;
    int choice_h;

    copy_str(e->gui_textbox, sizeof(e->gui_textbox), "gui/textbox.png");
    copy_str(e->gui_main_menu, sizeof(e->gui_main_menu), "gui/main_menu.png");
    copy_str(e->gui_choice_window, sizeof(e->gui_choice_window), "gui/choice_window.png");
    copy_str(e->gui_choice_idle, sizeof(e->gui_choice_idle), "gui/button/choice_idle_background.png");
    copy_str(e->gui_choice_hover, sizeof(e->gui_choice_hover), "gui/button/choice_hover_background.png");
    copy_str(e->gui_logo, sizeof(e->gui_logo), "gui/ClassOf09logo.png");
    e->gui_logo_x = 996;
    e->gui_logo_y = 87;
    e->gui_logo_w = 696;
    e->gui_logo_h = 474;

    e->gui_name_x = 360;
    e->gui_name_y = 806;
    e->gui_name_color = 0x279BE8;
    e->gui_dialogue_x = 320;
    e->gui_dialogue_y = 880;
    e->gui_dialogue_w = 1180;
    e->gui_dialogue_spacing = -5;
    e->gui_dialogue_color = 0x000000;
    e->gui_choice_x = 345;
    e->gui_choice_y = 324;
    e->gui_choice_w = 1185;
    e->gui_choice_h = 74;
    e->gui_choice_spacing = 33;
    e->gui_choice_idle_color = 0x000000;
    e->gui_choice_hover_color = 0xAEB88D;
    copy_str(e->gui_menu_music, sizeof(e->gui_menu_music), "audio/menu_piano.mp3");

    copy_str(e->font_paths[FONT_DIALOGUE], MAX_PATH, "handwriting2.ttf");
    copy_str(e->font_paths[FONT_NAME], MAX_PATH, "HELVETICA_EXTRA_COMPRESSED.TTF");
    copy_str(e->font_paths[FONT_CHOICE], MAX_PATH, "HELVETICA_LT_NARROW.TTF");
    copy_str(e->font_paths[FONT_MENU], MAX_PATH, "FUTURA.TTF");
    copy_str(e->font_paths[FONT_CHOICE_SMALL], MAX_PATH, "HELVETICA_LT_NARROW.TTF");
    copy_str(e->font_paths[FONT_CHOICE_TINY], MAX_PATH, "HELVETICA_LT_NARROW.TTF");
    copy_str(e->font_paths[FONT_CHOICE_MINI], MAX_PATH, "HELVETICA_LT_NARROW.TTF");
    e->font_sizes[FONT_DIALOGUE] = 50;
    e->font_sizes[FONT_NAME] = 60;
    e->font_sizes[FONT_CHOICE] = 60;
    e->font_sizes[FONT_MENU] = 60;
    e->font_sizes[FONT_CHOICE_SMALL] = 48;
    e->font_sizes[FONT_CHOICE_TINY] = 40;
    e->font_sizes[FONT_CHOICE_MINI] = 30;
    copy_str(e->font_paths[FONT_MONO], MAX_PATH, "joystix monospace.ttf");
    copy_str(e->font_paths[FONT_PHONE], MAX_PATH, "texting2007.ttf");
    copy_str(e->font_paths[FONT_BODY_TINY], MAX_PATH, "HELVETICA_LT_NARROW.TTF");
    copy_str(e->font_paths[FONT_TRACKER], MAX_PATH, "HELVETICA_LT_NARROW.TTF");
    copy_str(e->font_paths[FONT_TRACKER_SMALL], MAX_PATH, "HELVETICA_LT_NARROW.TTF");
    e->font_sizes[FONT_TRACKER_SMALL] = 16;
    e->font_sizes[FONT_MONO] = 48;
    e->font_sizes[FONT_PHONE] = 40;
    e->font_sizes[FONT_BODY_TINY] = 26;
    e->font_sizes[FONT_TRACKER] = 20;

    if (gui && gui->type == JL_OBJECT) {
        copy_str(e->gui_textbox, sizeof(e->gui_textbox), obj_str(gui, "textbox", e->gui_textbox));
        copy_str(e->gui_main_menu, sizeof(e->gui_main_menu), obj_str(gui, "main_menu", e->gui_main_menu));
        copy_str(e->gui_choice_window, sizeof(e->gui_choice_window),
                 obj_str(gui, "choice_window", e->gui_choice_window));
        copy_str(e->gui_choice_idle, sizeof(e->gui_choice_idle), obj_str(gui, "choice_idle", e->gui_choice_idle));
        copy_str(e->gui_choice_hover, sizeof(e->gui_choice_hover), obj_str(gui, "choice_hover", e->gui_choice_hover));

        dialogue = obj_get(gui, "dialogue");
        if (dialogue && dialogue->type == JL_OBJECT) {
            e->gui_dialogue_x = obj_int(dialogue, "x", e->gui_dialogue_x);
            e->gui_dialogue_y = obj_int(dialogue, "y", e->gui_dialogue_y);
            e->gui_dialogue_w = obj_int(dialogue, "w", e->gui_dialogue_w);
            e->gui_dialogue_spacing = obj_int(dialogue, "line_spacing", e->gui_dialogue_spacing);
            e->gui_dialogue_color = parse_color(obj_str(dialogue, "color", "#000000"), e->gui_dialogue_color);
            copy_str(e->font_paths[FONT_DIALOGUE], MAX_PATH, obj_str(dialogue, "font", "handwriting2.ttf"));
            e->font_sizes[FONT_DIALOGUE] = obj_int(dialogue, "size", 50);
        }
        name = obj_get(gui, "name");
        if (name && name->type == JL_OBJECT) {
            e->gui_name_x = obj_int(name, "x", e->gui_name_x);
            e->gui_name_y = obj_int(name, "y", e->gui_name_y);
            e->gui_name_color = parse_color(obj_str(name, "color", "#279be8"), e->gui_name_color);
            copy_str(e->font_paths[FONT_NAME], MAX_PATH,
                     obj_str(name, "font", "HELVETICA_EXTRA_COMPRESSED.TTF"));
            e->font_sizes[FONT_NAME] = obj_int(name, "size", 60);
        }
        choice = obj_get(gui, "choice");
        if (choice && choice->type == JL_OBJECT) {
            choice_w = obj_int(choice, "w", e->gui_choice_w);
            choice_h = obj_int(choice, "h", e->gui_choice_h);
            e->gui_choice_w = choice_w;
            e->gui_choice_h = choice_h;
            e->gui_choice_x = (int)(obj_num(choice, "xalign", 0.47) * (SCREEN_W - choice_w) + 0.5);
            e->gui_choice_y = (int)(obj_num(choice, "yalign", 0.3) * SCREEN_H + 0.5);
            e->gui_choice_spacing = obj_int(choice, "spacing", e->gui_choice_spacing);
            e->gui_choice_idle_color = parse_color(obj_str(choice, "idle", "#000000"), e->gui_choice_idle_color);
            e->gui_choice_hover_color = parse_color(obj_str(choice, "hover", "#AEB88D"), e->gui_choice_hover_color);
            copy_str(e->font_paths[FONT_CHOICE], MAX_PATH,
                     obj_str(choice, "font", "HELVETICA_LT_NARROW.TTF"));
            e->font_sizes[FONT_CHOICE] = obj_int(choice, "size", 60);
            copy_str(e->font_paths[FONT_CHOICE_SMALL], MAX_PATH, e->font_paths[FONT_CHOICE]);
            e->font_sizes[FONT_CHOICE_SMALL] = e->font_sizes[FONT_CHOICE] * 4 / 5;
            copy_str(e->font_paths[FONT_CHOICE_TINY], MAX_PATH, e->font_paths[FONT_CHOICE]);
            e->font_sizes[FONT_CHOICE_TINY] = e->font_sizes[FONT_CHOICE] * 2 / 3;
            copy_str(e->font_paths[FONT_CHOICE_MINI], MAX_PATH, e->font_paths[FONT_CHOICE]);
            e->font_sizes[FONT_CHOICE_MINI] = e->font_sizes[FONT_CHOICE] / 2;
        }
        menu = obj_get(gui, "menu");
        if (menu && menu->type == JL_OBJECT) {
            copy_str(e->font_paths[FONT_MENU], MAX_PATH, obj_str(menu, "font", "FUTURA.TTF"));
            e->font_sizes[FONT_MENU] = obj_int(menu, "size", 60);
            copy_str(e->gui_menu_music, sizeof(e->gui_menu_music),
                     obj_str(menu, "music", e->gui_menu_music));
        }
        logo = obj_get(gui, "logo");
        if (logo && logo->type == JL_OBJECT) {
            copy_str(e->gui_logo, sizeof(e->gui_logo), obj_str(logo, "file", e->gui_logo));
            e->gui_logo_x = obj_int(logo, "x", e->gui_logo_x);
            e->gui_logo_y = obj_int(logo, "y", e->gui_logo_y);
            e->gui_logo_w = obj_int(logo, "w", e->gui_logo_w);
            e->gui_logo_h = obj_int(logo, "h", e->gui_logo_h);
        }
    }

    copy_str(e->font_paths[FONT_SMALL], MAX_PATH, "HELVETICA_LT_NARROW.TTF");
    e->font_sizes[FONT_SMALL] = 34;
    copy_str(e->font_paths[FONT_TITLE], MAX_PATH, e->font_paths[FONT_MENU]);
    e->font_sizes[FONT_TITLE] = 84;
    copy_str(e->font_paths[FONT_BODY], MAX_PATH, "HELVETICA_LT_NARROW.TTF");
    e->font_sizes[FONT_BODY] = 40;
    copy_str(e->font_paths[FONT_SLOT], MAX_PATH, "FUTURA.TTF");
    e->font_sizes[FONT_SLOT] = 21;
}

static void load_fonts(Engine *e) {
    int i;
    for (i = 0; i < FONT_COUNT; i++) {
        ensure_font(e, i);
    }
}

static void load_splash(Engine *e);
#if defined(__PROSPERO__) || defined(__SWITCH__)
static void stop_webm(Engine *e);
static void stop_decode_thread(Engine *e);
#endif

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

int engine_init(Engine *e, const char *script_path, const char *game_dir) {
    const char *script;
    const char *game;
#if !defined(__PROSPERO__) && !defined(__SWITCH__)
    char dir[MAX_PATH];
#endif
    char *text;
    size_t len = 0;
    int i;
    int audio_flags = 0;

    if (!e) {
        return 0;
    }
    memset(e, 0, sizeof(*e));

#ifdef __PROSPERO__
    script = (script_path && script_path[0]) ? script_path : "/app0/assets/script.json";
    game = (game_dir && game_dir[0]) ? game_dir : DEFAULT_GAME_DIR;
    copy_str(e->save_dir, sizeof(e->save_dir), DEFAULT_SAVE_DIR);
#ifndef __PROSPERO__
    {
        const char *save_override = getenv("CLASS09_SAVE_DIR");
        if (save_override && save_override[0]) {
            copy_str(e->save_dir, sizeof(e->save_dir), save_override);
        }
    }
#endif
    copy_str(e->data_dir, sizeof(e->data_dir), DEFAULT_DATA_DIR);
#elif defined(__SWITCH__)
    script = (script_path && script_path[0]) ? script_path : DEFAULT_DATA_DIR "/script.json";
    game = (game_dir && game_dir[0]) ? game_dir : DEFAULT_GAME_DIR;
    copy_str(e->save_dir, sizeof(e->save_dir), DEFAULT_SAVE_DIR);
    copy_str(e->data_dir, sizeof(e->data_dir), DEFAULT_DATA_DIR);
#else
    script = (script_path && script_path[0]) ? script_path : "build/script.json";
    game = (game_dir && game_dir[0]) ? game_dir : DEFAULT_GAME_DIR;
    dir_name(script, dir, sizeof(dir));
    snprintf(e->save_dir, sizeof(e->save_dir), "%s/saves", dir);
    copy_str(e->data_dir, sizeof(e->data_dir), dir);
#endif
    copy_str(e->script_path, sizeof(e->script_path), script);
    copy_str(e->game_dir, sizeof(e->game_dir), game);
    c09_trace("engine: paths script=%s game=%s save=%s data=%s", e->script_path, e->game_dir,
              e->save_dir, e->data_dir);

    e->opt_music = 7;
    e->opt_voice = 10;
    e->hud = 1;
    e->running = 1;
    e->main_index = 0;
    e->frame_counter = 1;

    text = read_file(script, &len);
    if (!text) {
        c09_trace("engine: cannot read script file: %s", script);
        printf("class09: cannot read script file: %s\n", script);
        return 0;
    }
    c09_trace("engine: script read (%u bytes)", (unsigned)len);
    e->root = json_parse(text);
    free(text);
    if (!e->root || e->root->type != JL_OBJECT) {
        printf("class09: invalid JSON in %s\n", script);
        json_free(e->root);
        e->root = NULL;
        return 0;
    }
    e->ops = obj_get(e->root, "ops");
    e->labels = obj_get(e->root, "labels");
    e->gui = obj_get(e->root, "gui");
    e->endings = obj_get(e->root, "endings");
    if (!e->ops || e->ops->type != JL_ARRAY || !e->labels || e->labels->type != JL_OBJECT) {
        printf("class09: script file is missing ops or labels\n");
        json_free(e->root);
        e->root = NULL;
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        c09_trace("engine: SDL_Init failed: %s", SDL_GetError());
        printf("class09: SDL_Init failed: %s\n", SDL_GetError());
        json_free(e->root);
        e->root = NULL;
        return 0;
    }
    c09_trace("engine: SDL_Init ok");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
#ifdef __SWITCH__
    {
        Result boost = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
        c09_trace("switch: cpu boost rc=0x%x", (unsigned)boost);
    }
#endif

    {
#ifndef __PROSPERO__
        const char *capture = getenv("CLASS09_CAPTURE");
        if (capture && capture[0]) {
            e->capture_at = SDL_GetTicks() + (Uint32)atoi(capture);
        }
#endif
    }

#ifdef __PROSPERO__
    e->window = SDL_CreateWindow("Class of '09", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_W,
                                 SCREEN_H, SDL_WINDOW_SHOWN);
#elif defined(__SWITCH__)
    e->window = SDL_CreateWindow("Class of '09", 0, 0, SCREEN_W, SCREEN_H, 0);
#else
    e->window = SDL_CreateWindow("Class of '09", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                 SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#endif
    if (!e->window) {
        c09_trace("engine: SDL_CreateWindow failed: %s", SDL_GetError());
        printf("class09: SDL_CreateWindow failed: %s\n", SDL_GetError());
        engine_shutdown(e);
        return 0;
    }
    c09_trace("engine: window created");
    e->renderer = SDL_CreateRenderer(e->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!e->renderer) {
        e->renderer = SDL_CreateRenderer(e->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!e->renderer) {
        c09_trace("engine: SDL_CreateRenderer failed: %s", SDL_GetError());
        printf("class09: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        engine_shutdown(e);
        return 0;
    }
    c09_trace("engine: renderer created");
    {
        SDL_RendererInfo info;
        if (SDL_GetRendererInfo(e->renderer, &info) == 0 && info.name) {
            c09_trace("engine: renderer driver=%s", info.name);
        }
    }
    SDL_RenderSetLogicalSize(e->renderer, SCREEN_W, SCREEN_H);
    SDL_SetRenderDrawBlendMode(e->renderer, SDL_BLENDMODE_BLEND);
    SDL_ShowCursor(SDL_DISABLE);

    if (IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) == 0) {
        c09_trace("engine: IMG_Init failed: %s", IMG_GetError());
        printf("class09: IMG_Init failed: %s\n", IMG_GetError());
    }
    if (TTF_Init() != 0) {
        c09_trace("engine: TTF_Init failed: %s", TTF_GetError());
        printf("class09: TTF_Init failed: %s\n", TTF_GetError());
    }
    c09_trace("engine: image+font subsystems ready");

#ifdef MIX_INIT_MP3
    audio_flags |= MIX_INIT_MP3;
#endif
#ifdef MIX_INIT_OGG
    audio_flags |= MIX_INIT_OGG;
#endif
    Mix_Init(audio_flags);
    {
#ifdef __SWITCH__
        /* The Switch audio device runs at 48 kHz and homebrew CPU load can
         * starve the mixer callback; a larger buffer keeps voice steady. */
        int mix_rate = 48000;
        int mix_samples = 16384;
#else
        int mix_rate = 44100;
        int mix_samples = 4096;
#endif
        if (Mix_OpenAudio(mix_rate, MIX_DEFAULT_FORMAT, 2, mix_samples) == 0) {
            e->audio_ok = 1;
            Mix_AllocateChannels(8);
            Mix_VolumeMusic(MIX_MAX_VOLUME);
        } else {
            c09_trace("engine: Mix_OpenAudio failed: %s", Mix_GetError());
            printf("class09: Mix_OpenAudio failed: %s\n", Mix_GetError());
            e->audio_ok = 0;
        }
    }
    c09_trace("engine: audio ready (ok=%d)", e->audio_ok);

    ensure_save_dir(e);
    c09_trace("engine: save dir ready");
    load_seen(e);
    load_new_ending(e);
    load_gui(e);
    load_fonts(e);
    c09_trace("engine: gui+fonts loaded");
    apply_volumes(e);
    load_splash(e);
    c09_trace("engine: splash loaded");

    for (i = 0; i < SDL_NumJoysticks(); i++) {
        open_controller(e, i);
    }

    enter_main_menu(e);
    c09_trace("engine: main menu entered");
    return 1;
}

void engine_shutdown(Engine *e) {
    int i;
    if (!e) {
        return;
    }
#if defined(__PROSPERO__) || defined(__SWITCH__)
    stop_decode_thread(e);
    stop_webm(e);
    free(e->decode_buf[0]);
    free(e->decode_buf[1]);
    free(e->upload_buf);
    e->decode_buf[0] = NULL;
    e->decode_buf[1] = NULL;
    e->upload_buf = NULL;
    e->upload_size = 0;
    e->decode_buf_size = 0;
#endif
    if (e->intro_music) {
        Mix_HaltMusic();
        Mix_FreeMusic(e->intro_music);
        e->intro_music = NULL;
    }
    if (e->audio_ok) {
        Mix_HaltChannel(-1);
        Mix_HaltMusic();
    }
    stop_voice(e);
    if (e->voice_music) {
        Mix_FreeMusic(e->voice_music);
        e->voice_music = NULL;
    }
#ifdef __SWITCH__
    {
        int slot;
        for (slot = 0; slot < 4; slot++) {
            if (e->voice_chunks[slot]) {
                Mix_FreeChunk(e->voice_chunks[slot]);
                e->voice_chunks[slot] = NULL;
            }
        }
    }
    if (e->voice_line_chunk) {
        Mix_FreeChunk(e->voice_line_chunk);
        e->voice_line_chunk = NULL;
    }
    free(e->voice_pcm);
    e->voice_pcm = NULL;
    e->voice_pcm_size = 0;
#endif
    if (e->ambient_chunk) {
        Mix_FreeChunk(e->ambient_chunk);
        e->ambient_chunk = NULL;
    }
    if (e->music_chunk) {
        Mix_FreeChunk(e->music_chunk);
        e->music_chunk = NULL;
    }
    for (i = 0; i < e->chunk_count; i++) {
        if (e->chunk_cache[i].chunk) {
            Mix_FreeChunk(e->chunk_cache[i].chunk);
            e->chunk_cache[i].chunk = NULL;
        }
    }
    e->chunk_count = 0;
    for (i = 0; i < TEXT_CACHE; i++) {
        free_text_block(&e->text_cache[i]);
    }
    for (i = 0; i < e->tex_count; i++) {
        if (e->tex_cache[i].tex) {
            SDL_DestroyTexture(e->tex_cache[i].tex);
            e->tex_cache[i].tex = NULL;
        }
    }
    e->tex_count = 0;
    if (e->splash_tex) {
        SDL_DestroyTexture(e->splash_tex);
        e->splash_tex = NULL;
    }
    stop_percpop(e);
    for (i = 0; i < FONT_COUNT; i++) {
        if (e->fonts[i]) {
            TTF_CloseFont(e->fonts[i]);
            e->fonts[i] = NULL;
        }
    }
    if (e->pad) {
        SDL_GameControllerClose(e->pad);
        e->pad = NULL;
    }
    if (e->renderer) {
        SDL_DestroyRenderer(e->renderer);
        e->renderer = NULL;
    }
    if (e->window) {
        SDL_DestroyWindow(e->window);
        e->window = NULL;
    }
    if (e->audio_ok) {
        Mix_CloseAudio();
        e->audio_ok = 0;
    }
    Mix_Quit();
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    json_free(e->root);
    e->root = NULL;
    e->ops = NULL;
    e->labels = NULL;
    e->gui = NULL;
    e->endings = NULL;
}

/* ------------------------------------------------------------------------- */
/* frame loop                                                                */
/* ------------------------------------------------------------------------- */

static void update_waits(Engine *e) {
    int kind;
    if (e->mode != MODE_GAME || !e->waiting || !e->wait_auto || e->wait_deadline == 0) {
        return;
    }
    if (!time_reached(e->wait_deadline)) {
        return;
    }
    kind = e->wait_kind;
    e->waiting = 0;
    e->wait_auto = 0;
    e->wait_deadline = 0;
    e->wait_kind = WAIT_NONE;
    if (kind == WAIT_SAY || kind == WAIT_MOVIE) {
        stop_voice(e);
    }
}

static int path_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    fclose(file);
    return 1;
}

static int load_splash_dir(Engine *e, const char *subdir, int mode) {
    char manifest[SPLASH_PATH_MAX];
    char line[256];
    char frame[SPLASH_PATH_MAX];
    FILE *file;
    int frames = 0;
    int fps = 30;
    int width = SCREEN_W;
    int height = SCREEN_H;

    e->splash_active = 0;
    e->splash_mode = mode;
    if (!e->data_dir[0]) {
        return 0;
    }
    snprintf(e->splash_dir, sizeof(e->splash_dir), "%s/%s", e->data_dir, subdir);
    snprintf(manifest, sizeof(manifest), "%s/manifest.txt", e->splash_dir);
    file = fopen(manifest, "rb");
    if (!file) {
        c09_trace("movie: no manifest at %s", manifest);
        return 0;
    }
    while (fgets(line, sizeof(line), file)) {
        char key[64];
        int value;
        if (sscanf(line, "%63[^=]=%d", key, &value) == 2) {
            if (strcmp(key, "fps") == 0) {
                fps = value;
            } else if (strcmp(key, "frames") == 0) {
                frames = value;
            } else if (strcmp(key, "width") == 0) {
                width = value;
            } else if (strcmp(key, "height") == 0) {
                height = value;
            }
        }
    }
    fclose(file);

    if (fps < 1 || fps > 240) {
        fps = 30;
    }
    if (frames < 1 || frames > 20000) {
        c09_trace("movie: %s has bad frame count %d", subdir, frames);
        return 0;
    }
    if (width < 1 || width > 8192) {
        width = SCREEN_W;
    }
    if (height < 1 || height > 8192) {
        height = SCREEN_H;
    }
    snprintf(frame, sizeof(frame), "%s/frame_0000.jpg", e->splash_dir);
    if (!path_exists(frame)) {
        printf("class09: splash manifest present but %s is missing\n", frame);
        c09_trace("movie: missing first frame %s", frame);
        return 0;
    }
    e->splash_frames = frames;
    e->splash_fps = fps;
    e->splash_w = width;
    e->splash_h = height;
    e->splash_tex = NULL;
    e->splash_tex_index = -1;
    e->splash_start = SDL_GetTicks();
    e->splash_skip_at = e->splash_start + 2500;
    e->splash_active = 1;
    printf("class09: video %s: %d frames at %d fps (%dx%d)\n", subdir, frames, fps, width, height);
    return 1;
}

static void start_movie_audio(Engine *e);

#if defined(__PROSPERO__) || defined(__SWITCH__)
/* VP8 decoding runs on its own core so drawing never performs a decode. The
 * thread decodes to the wall clock; the render thread only copies the latest
 * converted RGBA frame. */
static void *webm_decode_thread(void *arg) {
    Engine *e = (Engine *)arg;
    WebmPlayer *player = (WebmPlayer *)e->decode_player;
    Uint32 stat_start = SDL_GetTicks();
    Uint32 decode_ms = 0;
    int decode_calls = 0;
    int frames = 0;
    int last_serial = 0;
#ifdef __SWITCH__
    svcSetThreadPriority(threadGetCurHandle(), 0x30);
#endif
    while (!e->decode_thread_stop && e->decode_player == player) {
        Uint32 now = SDL_GetTicks();
        Uint32 call_start;
        double t = (double)(now - e->decode_start) / 1000.0;
        double duration = webm_duration(player);
        const unsigned char *rgba = NULL;
        int width = 0;
        int height = 0;
        int serial = 0;
        if (e->decode_loop && duration > 0.01) {
            while (t >= duration) {
                t -= duration;
            }
        }
        if (t < 0.0) {
            t = 0.0;
        }
        /* Decode without the lock; only publishing the frame takes it. */
        call_start = SDL_GetTicks();
        if (webm_frame(player, t, &rgba, &width, &height, &serial) && rgba &&
            width > 0 && height > 0) {
            size_t need = (size_t)width * (size_t)height * 4u;
            if (serial != last_serial) {
                frames += serial - last_serial;
                last_serial = serial;
            }
            pthread_mutex_lock(&e->decode_mutex);
            if (e->decode_buf_size < need) {
                unsigned char *a = (unsigned char *)realloc(e->decode_buf[0], need);
                unsigned char *b = (unsigned char *)realloc(e->decode_buf[1], need);
                if (a) {
                    e->decode_buf[0] = a;
                }
                if (b) {
                    e->decode_buf[1] = b;
                }
                if (a && b) {
                    e->decode_buf_size = need;
                }
            }
            if (e->decode_buf_size >= need) {
                int back = 1 - e->decode_buf_index;
                memcpy(e->decode_buf[back], rgba, need);
                e->decode_buf_index = back;
                e->decode_buf_serial = serial;
                e->decode_buf_w = width;
                e->decode_buf_h = height;
            }
            pthread_mutex_unlock(&e->decode_mutex);
        }
        decode_ms += SDL_GetTicks() - call_start;
        decode_calls++;
        if (SDL_GetTicks() - stat_start >= 2000) {
            Uint32 wall = SDL_GetTicks() - stat_start;
            c09_trace("webm: %d frames in %u ms (%.1f wall/frame, %.1f decode/call, %d calls)",
                      frames, (unsigned)wall, frames > 0 ? (double)wall / (double)frames : 0.0,
                      decode_calls > 0 ? (double)decode_ms / (double)decode_calls : 0.0,
                      decode_calls);
            stat_start = SDL_GetTicks();
            decode_ms = 0;
            decode_calls = 0;
            frames = 0;
        }
        usleep(4000);
    }
    return NULL;
}

static void stop_decode_thread(Engine *e) {
    if (e->decode_thread_live) {
        e->decode_thread_stop = 1;
        pthread_join(e->decode_thread, NULL);
        e->decode_thread_live = 0;
    }
    e->decode_thread_stop = 0;
    e->decode_player = NULL;
}

static void start_decode_thread(Engine *e, void *player, Uint32 start, int loop) {
    pthread_attr_t attr;
    int rc;
    stop_decode_thread(e);
    if (!e->decode_mutex_ready) {
        pthread_mutex_init(&e->decode_mutex, NULL);
        e->decode_mutex_ready = 1;
    }
    e->decode_player = player;
    e->decode_start = start;
    e->decode_loop = loop;
    e->decode_thread_stop = 0;
    e->decode_buf_serial = -1;
    e->decode_buf_index = 0;
    pthread_attr_init(&attr);
    /* VP8 needs a real stack; the newlib default is too small for 1080p. */
    pthread_attr_setstacksize(&attr, 1024u * 1024u);
    rc = pthread_create(&e->decode_thread, &attr, webm_decode_thread, e);
    pthread_attr_destroy(&attr);
    if (rc == 0) {
        e->decode_thread_live = 1;
        c09_trace("webm: decode thread started");
    } else {
        e->decode_thread_live = 0;
        c09_trace("webm: decode thread failed rc=%d", rc);
    }
}

/* Uploads a video frame into a persistent streaming texture. Creating the
 * texture once and updating it avoids a surface + texture allocation for every
 * displayed frame. */
static void webm_upload(Engine *e, SDL_Texture **tex, int *tex_w, int *tex_h,
                        const unsigned char *src, int width, int height,
                        int serial, int *serial_out) {
    if (!src || width <= 0 || height <= 0) {
        return;
    }
    if (!*tex || *tex_w != width || *tex_h != height) {
        if (*tex) {
            SDL_DestroyTexture(*tex);
        }
        *tex = SDL_CreateTexture(e->renderer, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, width, height);
        if (*tex) {
            SDL_SetTextureBlendMode(*tex, SDL_BLENDMODE_BLEND);
        }
        *tex_w = width;
        *tex_h = height;
    }
    if (*tex) {
#ifdef __SWITCH__
        Uint32 upload_start = SDL_GetTicks();
#endif
        SDL_UpdateTexture(*tex, NULL, src, width * 4);
#ifdef __SWITCH__
        {
            Uint32 upload_ms = SDL_GetTicks() - upload_start;
            if (upload_ms >= 20) {
                c09_trace("webm: upload %u ms %dx%d", (unsigned)upload_ms, width, height);
            }
        }
#endif
        if (serial_out) {
            *serial_out = serial;
        }
    }
}

/* Pulls the newest decoded frame into the caller's persistent texture. */
static void webm_present(Engine *e, void *player_ptr, double t,
                         SDL_Texture **tex, int *tex_w, int *tex_h, int *serial) {
    WebmPlayer *player = (WebmPlayer *)player_ptr;
    if (!player) {
        return;
    }
    if (e->decode_thread_live && e->decode_mutex_ready) {
        int have = 0;
        int got_serial = 0;
        int width = 0;
        int height = 0;
        size_t need = 0;
        pthread_mutex_lock(&e->decode_mutex);
        if (e->decode_buf_serial != *serial && e->decode_buf_size > 0) {
            width = e->decode_buf_w;
            height = e->decode_buf_h;
            got_serial = e->decode_buf_serial;
            need = (size_t)width * (size_t)height * 4u;
            if (e->upload_size < need) {
                unsigned char *grown = (unsigned char *)realloc(e->upload_buf, need);
                if (grown) {
                    e->upload_buf = grown;
                    e->upload_size = need;
                }
            }
            if (e->upload_size >= need) {
                memcpy(e->upload_buf, e->decode_buf[e->decode_buf_index], need);
                have = 1;
            }
        }
        pthread_mutex_unlock(&e->decode_mutex);
        /* SDL calls can be slow on the console, so never make the decoder wait
         * for a texture update. */
        if (have) {
            webm_upload(e, tex, tex_w, tex_h, e->upload_buf, width, height, got_serial, serial);
        }
    } else {
        const unsigned char *rgba = NULL;
        int width = 0;
        int height = 0;
        int next = 0;
        if (webm_frame(player, t, &rgba, &width, &height, &next) && rgba) {
            webm_upload(e, tex, tex_w, tex_h, rgba, width, height, next, serial);
        }
    }
}

static void start_webm_audio(Engine *e, WebmPlayer *player) {
    char wav[SAVE_PATH_MAX];
    if (!e->audio_ok || !webm_has_audio(player)) {
        return;
    }
    ensure_save_dir(e);
    snprintf(wav, sizeof(wav), "%s/video_audio.wav", e->save_dir);
    if (webm_audio_to_wav(player, wav)) {
        e->intro_music = Mix_LoadMUS(wav);
        if (e->intro_music) {
            Mix_PlayMusic(e->intro_music, 0);
            Mix_VolumeMusic(e->opt_music * MIX_MAX_VOLUME / 10);
            c09_trace("webm: audio decoded");
        }
#ifndef __SWITCH__
        /* SDL_mixer streams WAV music from the file on the Switch, so keep it
         * until the video/music is released. */
        remove(wav);
#endif
    }
}

static int load_webm_movie(Engine *e, const char *path, int mode) {
    WebmPlayer *player;
    double duration;
    if (e->splash_webm) {
        webm_close((WebmPlayer *)e->splash_webm);
        e->splash_webm = NULL;
        e->splash_is_webm = 0;
    }
    player = webm_open(path);
    if (!player) {
        return 0;
    }
    if (!webm_has_video(player)) {
        webm_close(player);
        return 0;
    }
    e->splash_mode = mode;
    e->splash_is_webm = 1;
    e->splash_webm = player;
    e->splash_w = webm_width(player);
    e->splash_h = webm_height(player);
    duration = webm_duration(player);
    if (duration < 0.05) {
        duration = 0.05;
    }
    e->splash_fps = 30;
    e->splash_frames = (int)(duration * 30.0 + 0.5);
    if (e->splash_frames < 1) {
        e->splash_frames = 1;
    }
    e->splash_tex = NULL;
    e->splash_tex_index = -1;
    e->splash_tex_w = 0;
    e->splash_tex_h = 0;
    start_webm_audio(e, player);
    e->splash_start = SDL_GetTicks();
    e->splash_skip_at = e->splash_start + 2500;
    e->splash_active = 1;
    start_decode_thread(e, player, e->splash_start, 0);
    c09_trace("webm: %s %dx%d %.2fs", path, e->splash_w, e->splash_h, duration);
    return 1;
}

static void stop_webm(Engine *e) {
    if (e->splash_webm) {
        if (e->decode_thread_live && e->decode_player == e->splash_webm) {
            stop_decode_thread(e);
        }
        webm_close((WebmPlayer *)e->splash_webm);
        e->splash_webm = NULL;
    }
    e->splash_is_webm = 0;
}
#endif

static void load_splash(Engine *e) {
#if defined(__PROSPERO__) || defined(__SWITCH__)
    char path[SAVE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/logosplash.webm", e->game_dir);
    if (load_webm_movie(e, path, 0)) {
        return;
    }
#endif
    if (load_splash_dir(e, "videos/logosplash", 0)) {
        start_movie_audio(e);
    }
}

static void start_movie_audio(Engine *e) {
    char path[SPLASH_PATH_MAX];
    if (!e->audio_ok) {
        return;
    }
    snprintf(path, sizeof(path), "%s/audio.mp3", e->splash_dir);
    e->intro_music = Mix_LoadMUS(path);
    if (e->intro_music) {
        e->splash_start = SDL_GetTicks();
        e->splash_skip_at = e->splash_start + 2500;
        Mix_PlayMusic(e->intro_music, 0);
        Mix_VolumeMusic(e->opt_music * MIX_MAX_VOLUME / 10);
        c09_trace("movie: audio started (%s)", e->splash_dir);
    } else {
        c09_trace("movie: audio missing (%s, %s)", path, Mix_GetError());
    }
}

static int load_movie(Engine *e, const char *subdir, int mode) {
    if (!load_splash_dir(e, subdir, mode)) {
        return 0;
    }
    start_movie_audio(e);
    c09_trace("movie: playing %s, %d frames", subdir, e->splash_frames);
    return 1;
}

static int load_intro(Engine *e) {
#if defined(__PROSPERO__) || defined(__SWITCH__)
    char path[SAVE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/opening.webm", e->game_dir);
    if (load_webm_movie(e, path, 1)) {
        return 1;
    }
#endif
    return load_movie(e, "videos/opening", 1);
}

static int load_ending_video(Engine *e, const char *id) {
#if defined(__PROSPERO__) || defined(__SWITCH__)
    char path[SAVE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%send.webm", e->game_dir, id);
    if (load_webm_movie(e, path, 2)) {
        return 1;
    }
#endif
    {
        char subdir[64];
        snprintf(subdir, sizeof(subdir), "videos/endings/%s", id);
        return load_movie(e, subdir, 2);
    }
}

static void stop_percpop(Engine *e) {
    if (e->percpop_active) {
        c09_trace("percpop: stop after %u ms", (unsigned)(SDL_GetTicks() - e->percpop_start));
    }
    e->percpop_active = 0;
    e->percpop_index = -1;
#if defined(__PROSPERO__) || defined(__SWITCH__)
    if (e->percpop_webm) {
        if (e->decode_thread_live && e->decode_player == e->percpop_webm) {
            stop_decode_thread(e);
        }
        webm_close((WebmPlayer *)e->percpop_webm);
        e->percpop_webm = NULL;
    }
    e->percpop_is_webm = 0;
#endif
    if (e->percpop_tex) {
        SDL_DestroyTexture(e->percpop_tex);
        e->percpop_tex = NULL;
    }
    e->percpop_tex_w = 0;
    e->percpop_tex_h = 0;
}

static int load_percpop(Engine *e) {
    char manifest[SPLASH_PATH_MAX];
    char line[128];
    char frame[SPLASH_PATH_MAX];
    FILE *file;
    int frames = 0;
    int fps = 24;

    stop_percpop(e);
    if (!e->data_dir[0]) {
        return 0;
    }
#if defined(__PROSPERO__) || defined(__SWITCH__)
    {
        char path[SAVE_PATH_MAX];
        WebmPlayer *player;
        snprintf(path, sizeof(path), "%s/percpop.webm", e->game_dir);
        player = webm_open(path);
        if (player && webm_has_video(player)) {
            double duration = webm_duration(player);
            e->percpop_is_webm = 1;
            e->percpop_webm = player;
            e->percpop_w = webm_width(player);
            e->percpop_h = webm_height(player);
            e->percpop_duration = duration > 0.05 ? duration : 0.05;
            e->percpop_start = SDL_GetTicks();
            e->percpop_index = -1;
            e->percpop_active = 1;
            start_decode_thread(e, player, e->percpop_start, 0);
            c09_trace("percpop: webm %.2fs", duration);
            return 1;
        }
        if (player) {
            webm_close(player);
        }
    }
#endif
    snprintf(e->percpop_dir, sizeof(e->percpop_dir), "%s/videos/percpop", e->data_dir);
    snprintf(manifest, sizeof(manifest), "%s/manifest.txt", e->percpop_dir);
    file = fopen(manifest, "rb");
    if (!file) {
        c09_trace("percpop: no manifest at %s", manifest);
        return 0;
    }
    while (fgets(line, sizeof(line), file)) {
        char key[32];
        int value;
        if (sscanf(line, "%31[^=]=%d", key, &value) == 2) {
            if (strcmp(key, "fps") == 0) {
                fps = value;
            } else if (strcmp(key, "frames") == 0) {
                frames = value;
            }
        }
    }
    fclose(file);
    if (frames < 1 || frames > 20000 || fps < 1 || fps > 240) {
        c09_trace("percpop: bad manifest (%d frames, %d fps)", frames, fps);
        return 0;
    }
    snprintf(frame, sizeof(frame), "%s/frame_0000.jpg", e->percpop_dir);
    if (!path_exists(frame)) {
        c09_trace("percpop: missing %s", frame);
        return 0;
    }
    e->percpop_frames = frames;
    e->percpop_fps = fps;
    e->percpop_start = SDL_GetTicks();
    e->percpop_index = -1;
    e->percpop_active = 1;
    c09_trace("percpop: playing %d frames", frames);
    return 1;
}

static void draw_percpop(Engine *e) {
    Uint32 now;
    Uint32 elapsed;
    int index;
    int alpha;

    if (!e->percpop_active) {
        return;
    }
    now = SDL_GetTicks();
    elapsed = now - e->percpop_start;
    if (elapsed >= 7200) {
        c09_trace("percpop: timeout %u ms", (unsigned)elapsed);
        stop_percpop(e);
        return;
    }
    index = (int)((elapsed * (Uint32)e->percpop_fps) / 1000u);
    if (index >= e->percpop_frames) {
        index = e->percpop_frames - 1;
    }
#if defined(__PROSPERO__) || defined(__SWITCH__)
    if (e->percpop_is_webm && e->percpop_webm) {
        double seconds = (double)elapsed / 1000.0;
        webm_present(e, e->percpop_webm, seconds, &e->percpop_tex,
                     &e->percpop_tex_w, &e->percpop_tex_h, &e->percpop_index);
    } else
#endif
    if (!e->percpop_tex || e->percpop_index != index) {
        char path[SPLASH_PATH_MAX];
        SDL_Surface *surface;
        if (e->percpop_tex) {
            SDL_DestroyTexture(e->percpop_tex);
            e->percpop_tex = NULL;
        }
        snprintf(path, sizeof(path), "%s/frame_%04d.jpg", e->percpop_dir, index);
        surface = IMG_Load(path);
        if (surface) {
            e->percpop_tex = SDL_CreateTextureFromSurface(e->renderer, surface);
            SDL_FreeSurface(surface);
            if (e->percpop_tex) {
                SDL_SetTextureBlendMode(e->percpop_tex, SDL_BLENDMODE_BLEND);
            }
        }
        e->percpop_index = index;
    }
    if (e->percpop_tex) {
        SDL_Rect dst;
        dst.x = 0;
        dst.y = 0;
        dst.w = SCREEN_W;
        dst.h = SCREEN_H;
        /* Keep the cutscene background fully black for the whole clip, then
         * fade out at the very end instead of revealing gameplay under it. */
        {
            double duration = e->percpop_duration > 0.1 ? e->percpop_duration : 5.9;
            double seconds = (double)elapsed / 1000.0;
            const double fade = 0.35;
            if (seconds >= duration) {
                alpha = 0;
            } else if (seconds <= duration - fade) {
                alpha = 255;
            } else {
                alpha = (int)(255.0 * (duration - seconds) / fade);
            }
        }
        SDL_SetTextureAlphaMod(e->percpop_tex, (Uint8)alpha);
        SDL_RenderCopy(e->renderer, e->percpop_tex, NULL, &dst);
        SDL_SetTextureAlphaMod(e->percpop_tex, 255);
    }
}

static void stop_intro_audio(Engine *e) {
    if (!e->intro_music) {
        return;
    }
    if (e->audio_ok) {
        Mix_HaltMusic();
    }
    Mix_FreeMusic(e->intro_music);
    e->intro_music = NULL;
}

static void finish_splash(Engine *e) {
    if (!e->splash_active) {
        return;
    }
    e->splash_active = 0;
    if (e->splash_tex) {
        SDL_DestroyTexture(e->splash_tex);
        e->splash_tex = NULL;
    }
    e->splash_tex_index = -1;
    e->splash_tex_w = 0;
    e->splash_tex_h = 0;
#if defined(__PROSPERO__) || defined(__SWITCH__)
    stop_webm(e);
#endif
    if (e->splash_mode == 2) {
        c09_trace("ending video: ended after %u ms", (unsigned)(SDL_GetTicks() - e->splash_start));
        stop_intro_audio(e);
        e->splash_mode = 0;
        /* The video is over; show the ending text card next. */
        e->ending_active = 1;
        e->waiting = 1;
        e->wait_auto = 0;
        e->wait_kind = WAIT_ENDING;
    } else if (e->splash_mode == 1) {
        stop_intro_audio(e);
        e->splash_mode = 0;
    } else {
        stop_intro_audio(e);
        if (e->gui_menu_music[0]) {
            play_chunk(e, "music", e->gui_menu_music, 0, 1);
        }
    }
}

static void draw_splash(Engine *e) {
    int index;
    SDL_Texture *texture = e->splash_tex;

    if (!e->splash_active) {
        return;
    }
#if defined(__PROSPERO__) || defined(__SWITCH__)
    if (e->splash_is_webm && e->splash_webm) {
        double seconds = (double)(SDL_GetTicks() - e->splash_start) / 1000.0;
        webm_present(e, e->splash_webm, seconds, &e->splash_tex,
                     &e->splash_tex_w, &e->splash_tex_h, &e->splash_tex_index);
        texture = e->splash_tex;
    } else
#endif
    {
        index = (int)((SDL_GetTicks() - e->splash_start) * (Uint32)e->splash_fps / 1000);
        if (index < 0) {
            index = 0;
        }
        if (index >= e->splash_frames) {
            index = e->splash_frames - 1;
        }
        if (!e->splash_tex || e->splash_tex_index != index) {
            char path[SPLASH_PATH_MAX];
            SDL_Surface *surface;
            if (e->splash_tex) {
                SDL_DestroyTexture(e->splash_tex);
                e->splash_tex = NULL;
            }
            snprintf(path, sizeof(path), "%s/frame_%04d.jpg", e->splash_dir, index);
            surface = IMG_Load(path);
            if (surface) {
                e->splash_tex = SDL_CreateTextureFromSurface(e->renderer, surface);
                SDL_FreeSurface(surface);
                if (e->splash_tex) {
                    SDL_SetTextureBlendMode(e->splash_tex,
                                            e->splash_mode ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
                }
            } else {
                printf("class09: splash frame load failed: %s (%s)\n", path, IMG_GetError());
            }
            e->splash_tex_index = index;
        }
        texture = e->splash_tex;
    }

    fill_rect(e, 0, 0, SCREEN_W, SCREEN_H, 0x000000, 255);
    if (texture) {
        int sw = e->splash_w > 0 ? e->splash_w : SCREEN_W;
        int sh = e->splash_h > 0 ? e->splash_h : SCREEN_H;
        double scale = (double)SCREEN_W / (double)sw;
        SDL_Rect dst;
        if ((double)sh * scale > SCREEN_H) {
            scale = (double)SCREEN_H / (double)sh;
        }
        dst.w = (int)(sw * scale + 0.5);
        dst.h = (int)(sh * scale + 0.5);
        dst.x = (SCREEN_W - dst.w) / 2;
        dst.y = (SCREEN_H - dst.h) / 2;
        SDL_SetTextureAlphaMod(texture, 255);
        SDL_RenderCopy(e->renderer, texture, NULL, &dst);
    }
}

static void draw_menu_backdrop(Engine *e) {
    if ((e->mode == MODE_OPTIONS && e->options_return_mode == MODE_PAUSE) ||
        (e->mode == MODE_SAVE && e->save_return_mode == MODE_PAUSE) ||
        (e->mode == MODE_LOAD && e->load_return_mode == MODE_PAUSE)) {
        draw_world(e);
    } else {
        draw_main_menu(e);
    }
}

int engine_frame(Engine *e) {
    if (!e || !e->renderer) {
        return 0;
    }
    poll_events(e);
    if (!e->running) {
        return 0;
    }

    if (e->splash_active) {
        Uint32 elapsed = SDL_GetTicks() - e->splash_start;
        Uint32 duration = (Uint32)((double)e->splash_frames * 1000.0 / (double)e->splash_fps);
        if (elapsed >= duration) {
            finish_splash(e);
        }
    }

    if (e->mode == MODE_GAME && !e->splash_active) {
        update_waits(e);
        run_ops(e);
    }

    if (e->want_thumb && e->mode == MODE_PAUSE) {
        e->want_thumb = 0;
        SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
        SDL_RenderClear(e->renderer);
        draw_world(e);
        capture_pause_thumbnail(e);
    }

    SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
    SDL_RenderClear(e->renderer);

    if (e->splash_active) {
        draw_splash(e);
    } else {
        switch (e->mode) {
        case MODE_MENU:
            draw_main_menu(e);
            draw_new_message(e);
            break;
        case MODE_GAME:
            draw_world(e);
            break;
        case MODE_PAUSE:
            draw_world(e);
            draw_pause(e);
            break;
        case MODE_SAVE:
            draw_menu_backdrop(e);
            draw_slots(e);
            break;
        case MODE_LOAD:
            draw_menu_backdrop(e);
            draw_slots(e);
            break;
        case MODE_OPTIONS:
            draw_menu_backdrop(e);
            draw_options(e);
            break;
        case MODE_ABOUT:
            draw_main_menu(e);
            draw_about(e);
            break;
        case MODE_ENDINGS:
            fill_rect(e, 0, 0, SCREEN_W, SCREEN_H, 0x000000, 255);
            draw_endings(e);
            break;
        default:
            break;
        }
    }

    draw_toast(e);
    reset_texture_refs(e);
    if (e->capture_at && time_reached(e->capture_at)) {
        e->capture_at = 0;
        e->shot_requested = 1;
    }
    if (e->shot_requested) {
        e->shot_requested = 0;
        capture_frame(e);
    }
    SDL_RenderPresent(e->renderer);
#ifdef __SWITCH__
    {
        static Uint32 frame_log_at = 0;
        static Uint32 frame_log_count = 0;
        Uint32 frame_now = SDL_GetTicks();
        if (!frame_log_at) {
            frame_log_at = frame_now;
        }
        frame_log_count++;
        if (frame_now - frame_log_at >= 2000) {
            Uint32 span = frame_now - frame_log_at;
            c09_trace("frame: %.1f fps (%.1f ms)", 1000.0 * (double)frame_log_count / (double)span,
                      (double)span / (double)frame_log_count);
            frame_log_at = frame_now;
            frame_log_count = 0;
        }
    }
#endif
    return e->running;
}

