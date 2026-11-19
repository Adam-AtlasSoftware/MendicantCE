/* The campaign's third-person camera on foot (chase_camera.c, camera.view). */
#ifndef CHASE_CAMERA_H
#define CHASE_CAMERA_H

#include "math/real_math.h"

struct camera_command;

/* whether the local player's view on foot is the third-person camera now
(camera.view "third_person", in the campaign, alive, on foot, unzoomed, no
cinematic): the director then follows the player with following_camera's
proc, which asks chase_camera_command for its command */
boolean chase_camera_enabled(short local_player_index);
/* the following camera's command for it (FALSE: not enabled): the focus the
player's eye, moved over the shoulder (no farther than a wall allows), the
distance back of it (the observer keeps it out of walls along the same
line), and forward from that point through where the player's shot goes,
so that the reticle in the middle of the screen is on it */
boolean chase_camera_command(short local_player_index, struct camera_command *command);

#endif
