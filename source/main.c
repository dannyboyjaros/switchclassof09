/*
 * Class of '09 - Nintendo Switch entry point.
 *
 * Assets and saves live on the SD card:
 *   sdmc:/switch/class09/assets   (script.json, game/, videos/)
 *   sdmc:/switch/class09/saves    (created on first run)
 */
#include "engine.h"
#include "ps5_trace.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const char *script = argc > 1 ? argv[1] : NULL;
    const char *game_dir = argc > 2 ? argv[2] : NULL;
    Engine *engine;

    c09_trace_init();
    c09_trace("main: entered argc=%d script=%s game=%s", argc, script ? script : "(default)",
              game_dir ? game_dir : "(default)");

    engine = (Engine *)calloc(1, sizeof(Engine));
    if (!engine) {
        c09_trace("main: calloc failed");
        return 1;
    }
    c09_trace("main: engine allocated (%u bytes)", (unsigned)sizeof(Engine));

    if (!engine_init(engine, script, game_dir)) {
        c09_trace("main: engine_init failed");
        engine_shutdown(engine);
        free(engine);
        return 1;
    }
    c09_trace("main: engine_init ok");

    while (engine_frame(engine)) {
    }
    c09_trace("main: frame loop ended");

    engine_shutdown(engine);
    free(engine);
    c09_trace("main: shutdown complete");
    return 0;
}
