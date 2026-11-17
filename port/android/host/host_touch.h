/*
HOST_TOUCH.H

The on-screen touch controls' state between an overlay and the game
(host_touch.c): Android's (TouchControls.java, through JNI) and iOS's
(port/ios/touch_controls.m). The overlay's side; the game's is the host
imports host_touch_read, host_touch_look_read, host_touch_rumble,
host_touch_scene, host_touch_bindings and host_touch_picture_top
(guest/runtime/guest_host.h).
*/

#ifndef HOST_TOUCH_H
#define HOST_TOUCH_H

#include <stdint.h>

/* the controller the overlay holds: the SDL axes (left x, y, right x, y,
left trigger, right trigger: -32768..32767, 0..32767), then the SDL button
bits; a press is held for the game at least a tap's time */
void host_touch_set_state(const int32_t *state);
/* the view swipe, in the overlay's logical units with its sensitivity
applied, and the gyroscope's turn, radians of yaw (to the left) and pitch
(up), added to what the game reads next */
void host_touch_add_look(float dx, float dy);
void host_touch_add_gyro(float dx, float dy);
void host_touch_look_reset(void);
/* port 0's rumble as the device's vibration strength, 0 or 64..255 */
int host_touch_rumble_strength(void);
/* the profile's game control on each of the 16 controller buttons (-1:
none) into controls[16]; returns a count of its changes, 0 before the game
has sent it */
int host_touch_bindings_read(int32_t *controls);
/* the game's scene bits (port/linux/src/touch_input.c's _touch_scene_*);
0 until the game has read its controller once */
int host_touch_scene_read(void);
/* where the overlay wants the picture's top in a portrait window, in the
window's pixels (-1: none, the picture at the top), which the game reads
every frame through the host import host_touch_picture_top */
void host_touch_set_picture_top(int top);

#endif
