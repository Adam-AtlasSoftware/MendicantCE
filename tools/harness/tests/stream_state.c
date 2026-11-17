/*
STREAM_STATE.C (test)

The state file of port/linux/game/stream_state.c: its text (the state's
precedence, JSON escaping), written only when it changed and at most four
times a second, and written whole: to a file beside it, then renamed over
it, never opened for writing itself. test_stream_state.py takes the code
(under_test.inc).
*/

#include "harness.h"

#include <stddef.h>

#include "../../../port/linux/game/speedrun_state.h"

static char target[512];
static int opened_target_for_writing, renames, renamed_whole;

static FILE *test_fopen(char const *path, char const *mode)
{
	if (!strcmp(path, target) && strchr(mode, 'w'))
		opened_target_for_writing++;
	return fopen(path, mode);
}

/* (the file renamed into place holds a whole text: it ends with its line) */
static int test_rename(char const *from, char const *to)
{
	char text[512] = "";
	FILE *file = fopen(from, "rb");
	size_t length;

	if (file)
	{
		length = fread(text, 1, sizeof(text) - 1, file);
		text[length] = 0;
		fclose(file);
	}
	renames++;
	if (!strcmp(to, target) && strlen(text) && text[strlen(text) - 1] == '\n')
		renamed_whole++;
	return rename(from, to);
}

#define fopen test_fopen
#define rename test_rename
#define platform_log(...) ((void)0)

#include "under_test.inc"

#undef fopen
#undef rename

static char *file_text(void)
{
	static char text[512];
	FILE *file = fopen(target, "rb");
	size_t length = 0;

	text[0] = 0;
	if (file)
	{
		length = fread(text, 1, sizeof(text) - 1, file);
		fclose(file);
	}
	text[length] = 0;
	return text;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	struct stream_state_input input;
	struct stream_state_tracker tracker;
	char text[STREAM_STATE_TEXT_SIZE];

	snprintf(target, sizeof(target), "%s/game_state.json", argc > 2 ? argv[2] : ".");
	remove(target);
	memset(&input, 0, sizeof(input));
	memset(&tracker, 0, sizeof(tracker));
	input.flags = SPEEDRUN_STATE_FLAG_IN_GAME;
	input.map = "levels\\a10\\a10";
	input.mode = "campaign";
	input.seconds = 95;
	input.kills = 3;
	input.deaths = 1;

	/* the text, escaped, and the state's precedence */
	CASE("text")
	{
		stream_state_text(&input, text, sizeof(text));
		CHECK(!strcmp(text, "{\"state\":\"playing\",\"map\":\"levels\\\\a10\\\\a10\",\"mode\":\"campaign\","
			"\"clock\":95,\"score\":0,\"kills\":3,\"deaths\":1,\"practice\":false}\n"), "%s", text);
		input.practice = TRUE;
		stream_state_text(&input, text, sizeof(text));
		CHECK(strstr(text, "\"practice\":true}"), "%s", text);
		input.map = "a \"quoted\"\n map";
		stream_state_text(&input, text, sizeof(text));
		CHECK(strstr(text, "\"map\":\"a \\\"quoted\\\"\\u000a map\""), "%s", text);
		input.paused = TRUE;
		stream_state_text(&input, text, sizeof(text));
		CHECK(strstr(text, "\"state\":\"paused\""), "%s", text);
		input.flags |= SPEEDRUN_STATE_FLAG_CUTSCENE;
		stream_state_text(&input, text, sizeof(text));
		CHECK(strstr(text, "\"state\":\"cutscene\""), "%s", text);
		input.flags |= SPEEDRUN_STATE_FLAG_LOADING;
		stream_state_text(&input, text, sizeof(text));
		CHECK(strstr(text, "\"state\":\"loading\""), "%s", text);
		input.flags = SPEEDRUN_STATE_FLAG_MAIN_MENU | SPEEDRUN_STATE_FLAG_IN_GAME;
		stream_state_text(&input, text, sizeof(text));
		CHECK(strstr(text, "\"state\":\"menu\""), "%s", text);
		return 0;
	}
	/* written the first time; not again while the same; a change written
	only once 250 ms passed since the last write, then at once */
	CASE("changes")
	{
		stream_state_text(&input, text, sizeof(text));
		CHECK(stream_state_offer(&tracker, target, text, 1000), "the first not written");
		CHECK(!strcmp(file_text(), text), "the file: %s", file_text());
		CHECK(!stream_state_offer(&tracker, target, text, 5000), "written again unchanged");
		CHECK(renames == 1, "%d renames", renames);
		input.seconds = 96;
		stream_state_text(&input, text, sizeof(text));
		CHECK(stream_state_offer(&tracker, target, text, 5100), "a change not written");
		input.seconds = 97;
		stream_state_text(&input, text, sizeof(text));
		CHECK(!stream_state_offer(&tracker, target, text, 5200), "written 100 ms after the last");
		CHECK(strstr(file_text(), "\"clock\":96"), "the file: %s", file_text());
		CHECK(stream_state_offer(&tracker, target, text, 5350), "not written 250 ms after the last");
		CHECK(strstr(file_text(), "\"clock\":97"), "the file: %s", file_text());
		CHECK(renames == 3, "%d renames", renames);
		return 0;
	}
	/* every write a whole file renamed into place, the file itself never
	opened for writing, and nothing left beside it */
	CASE("atomic")
	{
		long index;
		char temporary[600];
		FILE *left;

		for (index = 0; index < 5; index++)
		{
			input.seconds = (unsigned long)index;
			stream_state_text(&input, text, sizeof(text));
			CHECK(stream_state_offer(&tracker, target, text, 1000 + (unsigned long)index * 300), "write %ld", index);
		}
		CHECK(opened_target_for_writing == 0, "the file opened for writing %d times", opened_target_for_writing);
		CHECK(renamed_whole == 5, "%d whole files renamed into place of 5", renamed_whole);
		snprintf(temporary, sizeof(temporary), "%s.tmp", target);
		left = fopen(temporary, "rb");
		CHECK(!left, "the temporary file left");
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", case_name);
	return 2;
}
