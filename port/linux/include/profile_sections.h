/*
PROFILE_SECTIONS.H

The port's CPU sections around the game's steps, the original's
profile_enter sections' kind (source/cseries/profile.h): timed in the
profiling build (configure.py --profile, port/linux/src/profile_trace.c)
and, with debug.timings on, in any build (profile.c's profile_frame_start;
the performance overlay's detailed view shows them, perf_overlay.c). Off,
each is the original's test of profile_global_enable before and after its
statement: no clock is read.
*/

#ifndef __PROFILE_SECTIONS_H
#define __PROFILE_SECTIONS_H

#include "cseries/profile.h"

/* these macros leave no trailing tokens: call sites have no trailing
semicolon. a call site may be an unbraced if-branch: profile_scope is one
braced statement. */
#define PROFILE_SECTION(variable, name) static struct profile_section variable = { name, NONE, TRUE };
#define profile_scope_enter(variable) profile_enter(variable)
#define profile_scope_exit(variable) profile_exit(variable)
#define profile_scope(variable, statement) { profile_enter(variable) statement profile_exit(variable) }

#endif
