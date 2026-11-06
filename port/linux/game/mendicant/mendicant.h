/* mendicant.h: MENDICANT AI's calls from the game's sources (port/linux/game/mendicant)

Every function does nothing, or exactly what the stock call would, unless
ai.mendicant.enabled is set and this machine runs the AI (the host of a
campaign or co-op game). */
#ifndef HALO_MENDICANT_H
#define HALO_MENDICANT_H

struct actor_type_definition;
struct firing_position_evaluation_context;
struct firing_position;
struct path_state;
struct charge_state_data;

/* mendicant_move_redirect's kinds: the five actor_move_* entries */
enum
{
	_mendicant_move_firing_position = 0,
	_mendicant_move_move_position,
	_mendicant_move_prop,
	_mendicant_move_halt,
	_mendicant_move_point,
};

/* mendicant_danger_react's answers */
enum
{
	_mendicant_danger_stock = 0, /* the game's own reaction */
	_mendicant_danger_dive,      /* the game's dive, in the direction written */
	_mendicant_danger_moved,     /* the soldier's own reaction is under way: no dive, no avoid action */
};

/* a map's start and end (ai.c) */
void mendicant_initialize_for_new_map(void);
void mendicant_dispose_from_old_map(void);

/* the settings screens wrote ai.mendicant.* (menu_functions.c's "port
settings save"): the knobs read during play take their new values now; those a
map's start sets up (on, the models, the seed, the bridge, training, the logs)
keep theirs until the next map */
void mendicant_settings_changed(void);

/* a saved game or checkpoint is loaded (game_state.c's after-load procs), the
AI on or off */
void mendicant_game_state_after_load(void);
/* a checkpoint or saved game is written (game_state.c's before-save procs):
no encounter is saved held active by the AI */
void mendicant_game_state_before_save(void);

/* the game tick (game_time.c around game_tick, game.c after the host's ai_update) */
void mendicant_game_tick_begin(void);
void mendicant_game_tick_end(void);
void mendicant_ai_update_end(void);

/* every decision of every actor (actor_types.c) */
void mendicant_decide_before(long actor_index, struct actor_type_definition const *definition);
void mendicant_decide_after(long actor_index);

/* actions (actions.c) */
void mendicant_action_control_after(long actor_index, short action);
boolean mendicant_veto_panic(long actor_index);
boolean mendicant_veto_berserk(long actor_index);
boolean mendicant_action_charge_setup(long actor_index, short goal, struct charge_state_data *state_data);
boolean mendicant_handle_lost_contact(long actor_index);
void mendicant_grenade_throw_checked(long actor_index, boolean throw_ordered_before);

/* firing positions (action_fight.c, action_uncover.c: in place of
actor_active_select_firing_position; action_guard.c: before its change) */
short mendicant_select_firing_position(long actor_index,
	struct firing_position_evaluation_context *evaluation_context,
	struct firing_position *best_firing_position, long *current_owner,
	struct path_state *area_path_state, boolean *area_path_state_valid);
void mendicant_guard_position(long actor_index, short *firing_position_index,
	long *previous_owner, boolean *cached_path_available);

/* movement (actor_moving.c): TRUE when an ordered actor went to its
waypoint instead; *result is then what the entry returns */
boolean mendicant_move_redirect(short kind, long actor_index, real_point3d const *point, boolean *result);

/* firing (actor_combat.c): each soldier's own firing personality, set on
his own control as the game starts a burst (its error and length, and the
radii its tracers start from and walk back by, before the sweep limit), a
pause or the wait before a first burst; mendicant_first_burst_delay returns
whether he fires at once (it may set a wait where the orders skipped it),
mendicant_fire_held is TRUE while he flinches from a hit */
void mendicant_burst_geometry(long actor_index, real *origin_radius, real *return_radius);
void mendicant_pause_started(long actor_index);
boolean mendicant_first_burst_delay(long actor_index, boolean fire_immediately);
boolean mendicant_fire_held(long actor_index);
void mendicant_combat_update_after(long actor_index);
/* the line of fire (ai.c): friendly ground vehicles with someone aboard, as
spheres (at most maximum); the count written */
short mendicant_line_of_fire_vehicles(long actor_index, short maximum,
	real_point3d *centers, real *radii, long *prop_indices);

/* a unit was hurt (units.c's unit_damage_aftermath): the fire log's "hit"
row, for the balance measures (nothing without ai.mendicant.fire_log); the
damage's category (a grenade's is 3) */
void mendicant_unit_damaged(long unit_index, long owner_object_index, short owner_team,
	real shield_damage, real body_damage, boolean lethal, short category);

/* grenade reactions (actor_perception.c, actions.c): each soldier notices a
live grenade in his own time, in place of the game's acknowledgement timer
(-1 not yet, 1 now, 0 the game's own), takes it in before he reacts
(TRUE: no avoidance yet), and reacts his own way once the game has found its
way out for him (inside the blast, on foot): mendicant_danger_react returns
_mendicant_danger_stock, _dive (escape_found, escape_direction and the
alignment written) or _moved */
short mendicant_danger_notice(long actor_index);
boolean mendicant_danger_hold(long actor_index);
/* near the blast but not in it, where the game would take the avoid action
(passive or pursuing) or stay put (fighting): TRUE when the soldier steps
clear his own way instead */
boolean mendicant_danger_avoid(long actor_index, short action_class);
short mendicant_danger_react(long actor_index, boolean *escape_found, short *escape_direction,
	real_vector2d *alignment_vector);

/* target weights (actor_perception.c) */
real mendicant_target_weight(long actor_index, long prop_index, real weight);

/* training without a picture or sound (main.c) */
boolean mendicant_skip_render_frame(void);
boolean mendicant_skip_sound_render(void);
/* (the mixer's thread, port/linux/src/dsound_sdl.c, which declares it itself) */
int mendicant_skip_mixer(void);

#endif
