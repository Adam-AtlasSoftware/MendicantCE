/*
GYRO_AIM.H

Aiming with a controller's gyroscope, and the flick stick (gyro_aim.c):
what a controller's motion sensors and right stick make of the player's
turn, apart from where the samples come from (xinput_sdl.c: SDL's gamepad
sensors, or the scripted player's samples, debug.test_input). Written from
Jibb Smart's published descriptions of the methods (GyroWiki: "Fundamentals
of Motion Aiming", "Player Space Gyro", "Soft Tiered Smoothing", "Flick
Stick"); no code of JoyShockMapper's or GamepadMotionHelpers' is used.

The controller's axes are SDL's (SDL_sensor.h): held level in front of
the player, x to its right, y up, z toward the player. The gyroscope's
rates are radians a second about them, counter-clockwise as seen from
their positive ends (about y: a turn to the left); the accelerometer's
meters a second squared, up at rest (gravity's reaction).

A turn is radians of the game's yaw (positive to the left) and pitch
(positive up), as the mouse's (halo_linux_mouse_look).
*/

#ifndef GYRO_AIM_H
#define GYRO_AIM_H

/* the settings (input.gyro_*): the game's degrees for each degree the
controller turns, side to side, and up and down of that; the turn's axis
from the gravity's direction (player space, held any way) or the
controller's own (local space); the smoothing of the smallest motions, its
strength (0 none; 1 the soft tiered smoothing's own thresholds and time,
which a strength scales: gyro_aim_smoothing) */
struct gyro_aim_settings
{
	float sensitivity;
	float vertical_sensitivity;
	int player_space;
	float smoothing;
};

/* the smoothing's strength of a setting's word (input.gyro_smoothing,
input.touch_gyro_smoothing): "off" none, "low" half, "medium" the
thresholds and time as they are, "high" twice; `otherwise` for another */
float gyro_aim_smoothing(const char *word, float otherwise);

struct gyro_aim
{
	/* the up direction in the controller's axes (a unit vector), once an
	accelerometer's sample has come; its last sample */
	float up[3];
	int up_known;
	float accelerometer[3];
	int accelerometer_known;
	/* the gyroscope's reading at rest (its bias), taken off every sample;
	whether one has been measured */
	float bias[3];
	int bias_known;
	/* the automatic calibration: the readings' recent means, and how long
	the controller has been still */
	float mean_gyro[3];
	float mean_accelerometer[3];
	int means_known;
	float still_seconds;
	/* a calibration asked for (gyro_aim_calibrate): the readings summed
	over its seconds so far */
	int calibrating;
	float calibration_sum[3];
	float calibration_seconds;
	/* the smoothing's share of the turn rate, yaw and pitch */
	float smoothed[2];
};

/* a controller's state as it begins (none of its readings known) */
void gyro_aim_reset(struct gyro_aim *aim);

/* the accelerometer's sample, for the next gyroscope sample */
void gyro_aim_accelerometer(struct gyro_aim *aim, const float accelerometer[3]);

/* the gyroscope's sample over seconds since the last: the turn it makes
(yaw and pitch, radians) for the settings; none while a calibration asked
for runs. The controller's state follows it (its up, its bias) even when the
turn is not wanted (the gyroscope paused: the caller drops it) */
void gyro_aim_sample(struct gyro_aim *aim, const float gyro[3], float seconds,
	const struct gyro_aim_settings *settings, float turn[2]);

/* a turn rate (yaw and pitch, radians a second, positive to the left and up)
over seconds: the turn it makes for the settings (their smoothing, its state
in smoothed, and the sensitivities). Every gyroscope's turn comes through
it: a controller's samples (gyro_aim_sample) and the touch controls' phone
or tablet (gyro_aim_turned) */
void gyro_aim_rate(const float rate[2], float smoothed[2], float seconds, const struct gyro_aim_settings *settings,
	float turn[2]);

/* a device's turn already made over seconds (yaw and pitch, radians), as the
touch controls hand the phone's or the tablet's over (touch_input.c): the turn
it makes for the settings, by gyro_aim_rate at its mean rate. None of it is
lost, whatever the time (none, or a gap) */
void gyro_aim_turned(const float turned[2], float smoothed[2], float seconds,
	const struct gyro_aim_settings *settings, float turn[2]);

/* measures the gyroscope at rest over the next GYRO_AIM_CALIBRATION_SECONDS
of samples, for its bias (the controller put down still) */
#define GYRO_AIM_CALIBRATION_SECONDS 1.5f
void gyro_aim_calibrate(struct gyro_aim *aim);

/* ---------- when the gyroscope's turn counts (input.gyro_aim) */

enum gyro_aim_mode
{
	GYRO_AIM_OFF,
	/* always */
	GYRO_AIM_ON,
	/* while input.gyro_button is held */
	GYRO_AIM_HOLD,
	/* always but while it is held (to bring the controller back to the
	middle, a ratchet) */
	GYRO_AIM_PAUSE,
	/* while the thumb is on the right stick (its capacitive touch) */
	GYRO_AIM_TOUCH,
};

/* whether the turn counts in a mode, the gyro button held or not, the right
stick touched or not (touched < 0: the controller cannot tell, and Thumb on
Stick turns nothing) */
int gyro_aim_active(int mode, int held, int touched);

/* whether a controller (its USB vendor and product) reports the touch of its
right stick: SDL 3.4's HIDAPI drivers of the Steam Deck's own controls
(28de:1205) and of the Steam Controller (2026: 28de:1302 to 1305) give it
as the gamepad's misc4 button (SDL_gamepad.c's mapping for them: misc4 is
their button 18, RIGHT_JOYSTICK_TOUCH, SDL_hidapi_steamdeck.c and
SDL_hidapi_steam_triton.c). Steam Input's virtual controller (28de:11ff)
has none */
int gyro_stick_touch_known(unsigned short vendor, unsigned short product);

/* whether a controller is the Steam Deck's own controls (28de:1205), whose
misc1 is the Quick Access button, Steam's own */
int gyro_steam_deck(unsigned short vendor, unsigned short product);

/* ---------- the flick stick */

struct flick_stick
{
	/* the stick past the rim's threshold, and its angle there (radians,
	0 up, clockwise) */
	int on_rim;
	float angle;
	/* the flick under way: its size (radians of yaw) and how much of it is
	done (0 to 1, 1 done) */
	float flick_size;
	float flick_done;
	/* the smoothing's share of the rim's turn rate */
	float smoothed;
};

void flick_stick_reset(struct flick_stick *flick);

/* the stick (x right, y up, -1 to 1) over seconds since the last: the yaw it
turns the player (radians). Pushed to the rim, the player turns to face
where it points, relative to the view, over flick_seconds (0: at once;
input.flick_stick_time, FLICK_STICK_SECONDS by default, at most
FLICK_STICK_SECONDS_MOST), always as far; turned around the rim, the player
turns as it does */
#define FLICK_STICK_SECONDS 0.1f
#define FLICK_STICK_SECONDS_MOST 0.25f
float flick_stick_update(struct flick_stick *flick, float x, float y, float seconds, float flick_seconds);

#endif
