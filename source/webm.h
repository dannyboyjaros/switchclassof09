#ifndef WEBM_H
#define WEBM_H

/*
 * Minimal WebM/Matroska player for the Class of '09 videos.
 *
 * The game ships VP8 video and Vorbis audio in .webm files.  This module
 * demuxes those containers and decodes them with libvpx + libvorbis.  It is
 * intentionally small: it understands EBML, Segment/Info/Tracks/Cluster,
 * SimpleBlock/Block, and Xiph lacing, which is all these files use.
 */

typedef struct WebmPlayer WebmPlayer;

WebmPlayer *webm_open(const char *path);
void webm_close(WebmPlayer *player);

int webm_has_video(const WebmPlayer *player);
int webm_has_audio(const WebmPlayer *player);
int webm_width(const WebmPlayer *player);
int webm_height(const WebmPlayer *player);
double webm_duration(const WebmPlayer *player);

/* Decode and return the video frame for time t (seconds).  On success sets
 * rgba to an RGBA8 buffer (R,G,B,A byte order), width/height, and a serial
 * that changes whenever a new frame is decoded. */
int webm_frame(WebmPlayer *player, double t, const unsigned char **rgba, int *width, int *height, int *serial);

/* The last converted frame, without decoding.  For a decoder thread: the
 * caller reads it while holding its own lock. */
int webm_current(WebmPlayer *player, const unsigned char **rgba, int *width, int *height, int *serial);

/* Decode the whole Vorbis track into a 16-bit PCM WAV file.  Returns 1 when
 * audio was written, 0 when the file has no audio or decoding failed. */
int webm_audio_to_wav(WebmPlayer *player, const char *path);

#endif /* WEBM_H */
