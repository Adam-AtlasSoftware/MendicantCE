#include "harness.h"

/* The controller's rumble strength (input.rumble_strength: halo_rumble_strength), applied where the motors are
written (source/input/input_xbox.c's input_port_write_rumble). 100%, the default, must write the game's own speeds,
to the bit; another strength scales both motors. */

typedef unsigned short word;
typedef void *HANDLE;
#define MAXIMUM_GAMEPADS 4
#define ERROR_SUCCESS 0
#define ERROR_IO_PENDING 997

typedef struct
{
	struct { unsigned long dwStatus; } Header;
	struct { word wLeftMotorSpeed, wRightMotorSpeed; } Rumble;
} XINPUT_FEEDBACK;
struct vibrate_data { word left_frequency, right_frequency; };

static struct
{
	XINPUT_FEEDBACK gamepad_feedbacks[MAXIMUM_GAMEPADS];
	boolean suppressed;
	HANDLE gamepad_handles[MAXIMUM_GAMEPADS];
	struct vibrate_data gamepad_rumbler_states[MAXIMUM_GAMEPADS];
} input_globals;

enum { INPUT_PORT_RUMBLE_REFRESH_MILLISECONDS = 33 };
static word input_port_rumble_written[MAXIMUM_GAMEPADS][2];
static unsigned long input_port_rumble_written_milliseconds[MAXIMUM_GAMEPADS];

/* the fake world: the player's strength, the clock, and the motors as written */
static unsigned long world_strength = 100;
static unsigned long world_now = 1000;
static word written[2];

static unsigned long halo_rumble_strength(short gamepad) { (void)gamepad; return world_strength; }
static boolean console_is_active(void) { return FALSE; }
static boolean game_time_get_paused(void) { return FALSE; }
static boolean game_in_progress(void) { return TRUE; }
static unsigned long system_milliseconds(void) { return world_now; }
static unsigned long XInputSetState(HANDLE handle, XINPUT_FEEDBACK *feedback)
{
	(void)handle;
	written[0] = feedback->Rumble.wLeftMotorSpeed;
	written[1] = feedback->Rumble.wRightMotorSpeed;
	return 0;
}

#include "under_test.inc"

/* the motors written for the game's speeds */
static void write(word left, word right)
{
	input_globals.gamepad_rumbler_states[0].left_frequency = left;
	input_globals.gamepad_rumbler_states[0].right_frequency = right;
	world_now += 100;
	input_port_write_rumble(0, TRUE);
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	static const word speeds[] = { 1, 7, 333, 12345, 32768, 65534, 65535 };
	unsigned index;

	input_globals.gamepad_handles[0] = &input_globals;
	CASE("default_is_the_game_s")
	{
		world_strength = 100;
		for (index = 0; index < sizeof(speeds) / sizeof(speeds[0]); index++)
		{
			write(speeds[index], speeds[sizeof(speeds) / sizeof(speeds[0]) - 1 - index]);
			CHECK(written[0] == speeds[index] && written[1] == speeds[sizeof(speeds) / sizeof(speeds[0]) - 1 - index],
				"the game's %u wrote %u at 100%%", speeds[index], written[0]);
		}
		return 0;
	}
	CASE("strength_scales_both")
	{
		world_strength = 50;
		write(65535, 40000);
		CHECK(written[0] == 32767 && written[1] == 20000, "50%% wrote %u, %u", written[0], written[1]);
		world_strength = 0;
		write(65535, 40000);
		CHECK(written[0] == 0 && written[1] == 0, "0%% wrote %u, %u", written[0], written[1]);
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
