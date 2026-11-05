/*
HRTF.C (test)

The HRTF of port/linux/src/dsound_sdl.c (test_hrtf.py takes the mixer as
under_test.inc, as test_mixer.py does) against the Xbox's: the filters and
delays the XDK DirectSound linked into the game's default.xbe looks up for
each direction once the game asks for its full HRTF (DirectSoundUseFullHRTF,
sound_dsound_xbox.c), as the MCPX's voice processor plays them. The Xbox's
figures below (xbox_figures) were measured on its own filters (see
test_hrtf.py on how to measure them again from a default.xbe); this holds
the figures only. Every figure is taken the same way from the Xbox's
responses and from ours, on a grid of directions every 15 degrees round and
every 10 from 40 below to 60 above, from impulse responses of 512 frames at
48 kHz:
	- "delays": the time between the ears (the lag of their greatest
	  cross-correlation below 1.5 kHz, to a sixteenth of a frame), to the
	  side, ahead of it and behind it; the delays in whole frames, as the
	  Xbox's;
	- "levels": the level between the ears in each octave from 125 Hz to 16
	  kHz, over every direction and to the side;
	- "notch": the deepest notch from 4 to 16 kHz ahead, below, level and
	  above, against the 2 to 16 kHz mean (each against the mean over all
	  directions: a directional transfer function);
	- "rear": what a sound behind loses above 2.5 kHz, against its low
	  frequencies, beside the direction mirrored ahead (the Xbox on stereo
	  played its table all round, its front mix bins alone).
"metrics" prints our figures beside the Xbox's; "measure" prints the
figures of responses given (raw floats: for each direction of the grid,
elevation by elevation from below and round from ahead to the right, 512
of the left ear then 512 of the right) in the form of xbox_figures.
*/

#include "harness.h"

#include <math.h>
#include <pthread.h>
#include <stddef.h>

typedef int BOOL;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long ULONG;
typedef void *LPVOID;
typedef void (*LPFNXMEDIAOBJECTCALLBACK)(void *, void *, DWORD);
typedef struct { void *lpVtbl; } IDirectSoundStream;
typedef struct { void *pvBuffer; DWORD dwMaxSize; } XMEDIAPACKET;
typedef struct
{
	LONG lRoom, lRoomHF;
	float flRoomRolloffFactor, flDecayTime, flDecayHFRatio;
	LONG lReflections;
	float flReflectionsDelay;
	LONG lReverb;
	float flReverbDelay, flDiffusion, flDensity, flHFReference;
} DSI3DL2LISTENER;
#define DSBVOLUME_MIN (-10000)
#define DS3DMODE_NORMAL 0x00000000
#define DS3DMODE_HEADRELATIVE 0x00000001
#define DS3DMODE_DISABLE 0x00000002

typedef unsigned long long Uint64;
#define platform_log(...) ((void)0)
#define SDL_GetTicksNS() 0ull

static const char *config_string(const char *name)
{
	(void)name;
	return "";
}

static void voice_audio_mix(float *output, unsigned long frames)
{
	(void)output;
	(void)frames;
}

#include "under_test.inc"

#define PI 3.14159265358979323846

/* ---------- the grid and the responses */

#define FRAMES 512
#define BINS (FRAMES / 2 + 1)
#define AZIMUTHS 24
#define ELEVATIONS 11
#define DIRECTIONS (AZIMUTHS * ELEVATIONS)
#define OCTAVES 8

static const double octaves[OCTAVES] = { 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

/* direction index's azimuth (degrees round to the right from ahead) and
elevation */
static int azimuth_of(int direction) { return direction % AZIMUTHS * 15; }
static int elevation_of(int direction) { return direction / AZIMUTHS * 10 - 40; }
static int direction_at(int azimuth, int elevation)
{
	return (elevation + 40) / 10 * AZIMUTHS + ((azimuth % 360 + 360) % 360) / 15;
}

/* our response from direction (both ears, FRAMES each), through the HRTF
as the mixer has it (hrtf_place, hrtf_process), 3 units away; its
parameters in placed */
static void our_response(int direction, float *left, float *right, struct hrtf_parameters *placed)
{
	static float impulse[FRAMES], output[FRAMES * 2];
	struct sdl_stream *stream = calloc(1, sizeof(*stream));
	struct hrtf_parameters parameters;
	double azimuth = azimuth_of(direction) * PI / 180.0, elevation = elevation_of(direction) * PI / 180.0;
	unsigned long n;

	hrtf_initialize();
	memset(&parameters, 0, sizeof(parameters));
	hrtf_place((float)(3.0 * sin(azimuth) * cos(elevation)), (float)(3.0 * sin(elevation)),
		(float)(3.0 * cos(azimuth) * cos(elevation)), 3.0f, &parameters);
	parameters.gain = 1.0f;
	if (placed)
		*placed = parameters;
	stream->hrtf_current = parameters;
	memset(impulse, 0, sizeof(impulse));
	memset(output, 0, sizeof(output));
	impulse[0] = 1.0f;
	hrtf_process(stream, impulse, output, FRAMES, FRAMES, &parameters, 1.0f, 0.0f);
	for (n = 0; n < FRAMES; n++)
	{
		left[n] = output[n * 2];
		right[n] = output[n * 2 + 1];
	}
	free(stream);
}

/* ---------- the figures */

struct figures
{
	/* the delay between the ears (ms; the left later positive) level, 30
	below and 30 above, at 30, 60, 90, 120 and 150 degrees to the right */
	double delay[3][5];
	/* the level between the ears in each octave (dB, the right over the
	left): its mean over every direction (of its size), and level at 45, 90
	and 135 degrees to the right */
	double level_mean[OCTAVES];
	double level[3][OCTAVES];
	/* the notch ahead: 30 below, level, 30 above (its frequency, Hz, and
	its depth, dB) */
	double notch_hz[3], notch_db[3];
	/* what is lost above 2.5 kHz behind (dB, against the mirrored front):
	level at 135, 150 and 180 degrees, and 30 above and below at 180 */
	double rear[5];
};

struct spectra
{
	double left[BINS], right[BINS]; /* power, dB */
};

static double bin_hertz(int bin) { return bin * (double)OUTPUT_RATE / FRAMES; }

static void spectrum(const float *x, double *power_db, double *re_out, double *im_out)
{
	int k, n;

	for (k = 0; k < BINS; k++)
	{
		double re = 0.0, im = 0.0;

		for (n = 0; n < FRAMES; n++)
		{
			re += x[n] * cos(2.0 * PI * k * n / FRAMES);
			im -= x[n] * sin(2.0 * PI * k * n / FRAMES);
		}
		power_db[k] = 10.0 * log10(re * re + im * im + 1e-20);
		if (re_out)
		{
			re_out[k] = re;
			im_out[k] = im;
		}
	}
}

static double band_db(const double *power_db, double center)
{
	double low = center / sqrt(2.0), high = center * sqrt(2.0), sum = 0.0;
	int k, count = 0;

	if (high > 22000.0)
		high = 22000.0;
	for (k = 0; k < BINS; k++)
	{
		if (bin_hertz(k) >= low && bin_hertz(k) < high)
		{
			sum += pow(10.0, power_db[k] / 10.0);
			count++;
		}
	}
	return 10.0 * log10(sum / count + 1e-20);
}

/* the lag (ms) of the left ear's greatest cross-correlation with the
right's below 1.5 kHz, to a sixteenth of a frame */
static double ears_delay_ms(const double *lre, const double *lim, const double *rre, const double *rim)
{
	double best = -1e300, lag = 0.0;
	int step;

	for (step = -48 * 16; step <= 48 * 16; step++)
	{
		double tau = step / 16.0, sum = 0.0;
		int k;

		for (k = 0; k < BINS && bin_hertz(k) < 1500.0; k++)
		{
			/* L conj(R) e^(j 2 pi k tau / N) */
			double xr = lre[k] * rre[k] + lim[k] * rim[k], xi = lim[k] * rre[k] - lre[k] * rim[k];
			double c = cos(2.0 * PI * k * tau / FRAMES), s = sin(2.0 * PI * k * tau / FRAMES);

			sum += (k ? 2.0 : 1.0) * (xr * c - xi * s);
		}
		if (sum > best)
		{
			best = sum;
			lag = tau;
		}
	}
	return lag * 1000.0 / OUTPUT_RATE;
}

/* the figures of responses (for each direction, FRAMES of the left ear
then FRAMES of the right) */
static struct figures measure(const float *responses)
{
	static struct spectra s[DIRECTIONS];
	static double mono[DIRECTIONS][BINS], ild[DIRECTIONS][OCTAVES], delay[DIRECTIONS], high[DIRECTIONS];
	static double common[BINS];
	double re[2][BINS], im[2][BINS];
	struct figures f;
	int d, k, b, i;

	memset(&f, 0, sizeof(f));
	memset(common, 0, sizeof(common));
	for (d = 0; d < DIRECTIONS; d++)
	{
		spectrum(responses + d * FRAMES * 2, s[d].left, re[0], im[0]);
		spectrum(responses + d * FRAMES * 2 + FRAMES, s[d].right, re[1], im[1]);
		delay[d] = ears_delay_ms(re[0], im[0], re[1], im[1]);
		for (b = 0; b < OCTAVES; b++)
			ild[d][b] = band_db(s[d].left, octaves[b]) - band_db(s[d].right, octaves[b]);
		for (k = 0; k < BINS; k++)
			common[k] += (pow(10.0, s[d].left[k] / 10.0) + pow(10.0, s[d].right[k] / 10.0)) / (2.0 * DIRECTIONS);
	}
	for (d = 0; d < DIRECTIONS; d++)
	{
		double low_sum = 0.0, high_sum = 0.0;
		int lows = 0, highs = 0;

		for (k = 0; k < BINS; k++)
		{
			double c = 10.0 * log10(common[k]);

			mono[d][k] = 10.0 * log10((pow(10.0, (s[d].left[k] - c) / 10.0) + pow(10.0, (s[d].right[k] - c) / 10.0)) / 2.0);
			if (bin_hertz(k) >= 100.0 && bin_hertz(k) <= 1000.0)
			{
				low_sum += mono[d][k];
				lows++;
			}
			if (bin_hertz(k) >= 2500.0 && bin_hertz(k) <= 16000.0)
			{
				high_sum += mono[d][k];
				highs++;
			}
		}
		high[d] = high_sum / highs - low_sum / lows;
	}
	for (i = 0; i < 3; i++)
	{
		for (b = 0; b < 5; b++)
			f.delay[i][b] = delay[direction_at(30 + 30 * b, i == 0 ? 0 : (i == 1 ? -30 : 30))];
		for (b = 0; b < OCTAVES; b++)
			f.level[i][b] = -ild[direction_at(45 + 45 * i, 0)][b];
	}
	for (b = 0; b < OCTAVES; b++)
	{
		for (d = 0; d < DIRECTIONS; d++)
			f.level_mean[b] += fabs(ild[d][b]) / DIRECTIONS;
	}
	for (i = 0; i < 3; i++)
	{
		int at = direction_at(0, -30 + 30 * i), lowest = -1, count = 0;
		double sum = 0.0;

		for (k = 0; k < BINS; k++)
		{
			double hz = bin_hertz(k);

			if (hz >= 4000.0 && hz <= 16000.0 && (lowest < 0 || mono[at][k] < mono[at][lowest]))
				lowest = k;
			if (hz >= 2000.0 && hz <= 16000.0)
			{
				sum += mono[at][k];
				count++;
			}
		}
		f.notch_hz[i] = bin_hertz(lowest);
		f.notch_db[i] = mono[at][lowest] - sum / count;
	}
	{
		static const int rear_at[5][2] = { { 135, 0 }, { 150, 0 }, { 180, 0 }, { 180, -30 }, { 180, 30 } };

		for (i = 0; i < 5; i++)
		{
			f.rear[i] = high[direction_at(rear_at[i][0], rear_at[i][1])] -
				high[direction_at(180 - rear_at[i][0], rear_at[i][1])];
		}
	}
	return f;
}

static float *our_responses(void)
{
	float *responses = malloc(sizeof(float) * FRAMES * 2 * DIRECTIONS);
	int d;

	for (d = 0; d < DIRECTIONS; d++)
		our_response(d, responses + d * FRAMES * 2, responses + d * FRAMES * 2 + FRAMES, NULL);
	return responses;
}

/* the Xbox's figures, measured on its own filters and delays (the table
DirectSoundUseFullHRTF has DirectSound look up), all round (its table's
own, which the Xbox on stereo played) */
static const struct figures xbox_figures =
{
	{ { 0.275, 0.651, 0.724, 0.643, 0.255 }, { 0.241, 0.574, 0.673, 0.574, 0.229 }, { 0.260, 0.613, 0.684, 0.604, 0.263 } },
	{ 2.1, 2.2, 2.9, 5.3, 6.7, 8.5, 12.9, 14.9 },
	{ { 2.5, 2.9, 4.4, 9.5, 9.6, 14.4, 16.8, 17.4 },
	  { 3.9, 4.0, 4.6, 5.6, 6.2, 14.8, 23.1, 24.7 },
	  { 2.1, 2.6, 4.4, 9.3, 6.8, 5.4, 18.0, 16.5 } },
	{ 6094, 9938, 15094 }, { -5.6, -6.3, -13.8 },
	{ -4.5, -9.7, -12.0, -10.2, -7.5 }
};

static void print_figures(const char *name, const struct figures *f)
{
	int i, b;

	printf("%s\n  delay ms (30 60 90 120 150 degrees):", name);
	for (i = 0; i < 3; i++)
	{
		printf(i ? " |" : "");
		for (b = 0; b < 5; b++)
			printf(" %.3f", f->delay[i][b]);
	}
	printf("\n  level dB (125 Hz to 16 kHz) mean:");
	for (b = 0; b < OCTAVES; b++)
		printf(" %.1f", f->level_mean[b]);
	for (i = 0; i < 3; i++)
	{
		printf("\n    at %d:", 45 + 45 * i);
		for (b = 0; b < OCTAVES; b++)
			printf(" %.1f", f->level[i][b]);
	}
	printf("\n  notch (30 below, level, 30 above):");
	for (i = 0; i < 3; i++)
		printf(" %.0f Hz %.1f dB", f->notch_hz[i], f->notch_db[i]);
	printf("\n  rear dB (135, 150, 180; 180 30 below, above):");
	for (i = 0; i < 5; i++)
		printf(" %.1f", f->rear[i]);
	printf("\n");
}

static float *read_floats(const char *path, unsigned long count)
{
	FILE *file = fopen(path, "rb");
	float *values = malloc(count * sizeof(float));

	if (!file || fread(values, sizeof(float), count, file) != count)
	{
		fprintf(stderr, "could not read %lu floats from %s\n", count, path);
		exit(2);
	}
	fclose(file);
	return values;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	static struct figures ours;
	int i, b;

	if (strcmp(case_name, "measure"))
	{
		float *responses = our_responses();

		ours = measure(responses);
		free(responses);
	}
	CASE("metrics")
	{
		print_figures("ours", &ours);
		print_figures("the Xbox's", &xbox_figures);
		return 0;
	}
	CASE("measure")
	{
		float *responses = read_floats(argv[2], (unsigned long)FRAMES * 2 * DIRECTIONS);
		struct figures f = measure(responses);

		printf("{\n\t{");
		for (i = 0; i < 3; i++)
			printf("%s{ %.3f, %.3f, %.3f, %.3f, %.3f }", i ? ", " : " ", f.delay[i][0], f.delay[i][1], f.delay[i][2],
				f.delay[i][3], f.delay[i][4]);
		printf(" },\n\t{");
		for (b = 0; b < OCTAVES; b++)
			printf("%s%.1f", b ? ", " : " ", f.level_mean[b]);
		printf(" },\n\t{");
		for (i = 0; i < 3; i++)
		{
			printf("%s{", i ? ",\n\t  " : " ");
			for (b = 0; b < OCTAVES; b++)
				printf("%s%.1f", b ? ", " : " ", f.level[i][b]);
			printf(" }");
		}
		printf(" },\n\t{ %.0f, %.0f, %.0f }, { %.1f, %.1f, %.1f },\n\t{ %.1f, %.1f, %.1f, %.1f, %.1f }\n}\n",
			f.notch_hz[0], f.notch_hz[1], f.notch_hz[2], f.notch_db[0], f.notch_db[1], f.notch_db[2], f.rear[0],
			f.rear[1], f.rear[2], f.rear[3], f.rear[4]);
		return 0;
	}

	/* the time between the ears within 0.06 ms of the Xbox's (its filters
	add a little to its whole frames, as the head's shadow adds to ours);
	every delay a whole frame, as the Xbox's, and to the side 30 to 32
	frames, as its 30 (0.625 ms) */
	CASE("delays")
	{
		int d;

		for (i = 0; i < 3; i++)
		{
			for (b = 0; b < 5; b++)
			{
				CHECK(fabs(ours.delay[i][b] - xbox_figures.delay[i][b]) < 0.06, "the delay between the ears at %d "
					"degrees round, %d up: %.3f ms, the Xbox's %.3f", 30 + 30 * b, i == 0 ? 0 : (i == 1 ? -30 : 30),
					ours.delay[i][b], xbox_figures.delay[i][b]);
			}
		}
		for (d = 0; d < DIRECTIONS; d++)
		{
			static float left[FRAMES], right[FRAMES];
			struct hrtf_parameters placed;

			our_response(d, left, right, &placed);
			CHECK(placed.delay[0] == floorf(placed.delay[0]) && placed.delay[1] == floorf(placed.delay[1]),
				"at %d degrees round, %d up, the ears' delays %g and %g frames, not whole", azimuth_of(d),
				elevation_of(d), placed.delay[0], placed.delay[1]);
			if (azimuth_of(d) == 90 && elevation_of(d) == 0)
			{
				CHECK(placed.delay[0] >= 30.0f && placed.delay[0] <= 32.0f && placed.delay[1] == 0.0f,
					"to the right, the left ear %g frames later, not 30 to 32", placed.delay[0]);
			}
		}
		return 0;
	}
	/* the level between the ears: over every direction within 1.5 dB of the
	Xbox's in each octave, and at 45, 90 and 135 degrees within 2 dB to 500
	Hz and 8 dB above (the Xbox's 31 taps have peaks and dips a shelf has
	not, and behind the side differ from ahead of it, where Brown and Duda's
	shadow is the same at 45 degrees from the ear's axis either way) */
	CASE("levels")
	{
		for (b = 0; b < OCTAVES; b++)
		{
			CHECK(fabs(ours.level_mean[b] - xbox_figures.level_mean[b]) < 1.5, "the level between the ears at %.0f Hz "
				"over every direction: %.1f dB, the Xbox's %.1f", octaves[b], ours.level_mean[b],
				xbox_figures.level_mean[b]);
			for (i = 0; i < 3; i++)
			{
				CHECK(fabs(ours.level[i][b] - xbox_figures.level[i][b]) < (octaves[b] <= 500 ? 2.0 : 8.0),
					"the level between the ears at %.0f Hz, %d degrees round: %.1f dB, the Xbox's %.1f", octaves[b],
					45 + 45 * i, ours.level[i][b], xbox_figures.level[i][b]);
			}
		}
		return 0;
	}
	/* the notch ahead within 1.5 kHz of the Xbox's, below, level and above,
	and at least 3 dB deep */
	CASE("notch")
	{
		for (i = 0; i < 3; i++)
		{
			CHECK(fabs(ours.notch_hz[i] - xbox_figures.notch_hz[i]) < 1500.0 && ours.notch_db[i] < -3.0,
				"the notch ahead, %d up: %.0f Hz %.1f dB, the Xbox's %.0f Hz %.1f dB", -30 + 30 * i, ours.notch_hz[i],
				ours.notch_db[i], xbox_figures.notch_hz[i], xbox_figures.notch_db[i]);
		}
		return 0;
	}
	/* behind, the high frequencies within 3 dB of the Xbox's loss */
	CASE("rear")
	{
		static const char *at[5] = { "135 degrees", "150 degrees", "behind", "behind, 30 below", "behind, 30 above" };

		for (i = 0; i < 5; i++)
		{
			CHECK(fabs(ours.rear[i] - xbox_figures.rear[i]) < 3.0, "%s, above 2.5 kHz %+.1f dB of the front's, the "
				"Xbox's %+.1f", at[i], ours.rear[i], xbox_figures.rear[i]);
		}
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
