/*
STREAM_STATE.H

The game's state in a small JSON file for stream overlays and scene
switchers (display.state_file, off by default: stream_state.c).
*/

#ifndef STREAM_STATE_H
#define STREAM_STATE_H

/* the file's name in the data root */
#define STREAM_STATE_FILE_NAME "game_state.json"

/* each frame, after the speedrun state block's (main.c): the file written
again if the state changed, at most four times a second */
void stream_state_frame(void);

#endif
