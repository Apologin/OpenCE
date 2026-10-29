/*
AUTO_GAME.C

A game that sets itself up without the menus, for a launcher that hosts and
joins for its players (a web page's room, say): auto.game in config.toml,
HALO_AUTO_GAME:

- "host" hosts a system link game on auto.map with the game type
  auto.game_type, as the pregame screen's fast setup does but with that map
  and game type from the start (fast setup's own is the playlist's Derelict,
  which every machine would then need), adds this machine's player and starts
  the game auto.start seconds after hosting, once that player is in (a
  negative auto.start: when the launcher calls auto_game_start_now, as a
  host's Start button might). Once a game is over and its scores have been
  shown a while, the next, the same;
- "local" the same with a local (split screen) game of one player, as
  Multiplayer > Split Screen sets one up (the bots' games, bots.count);
- "join" searches for system link games and joins the first it finds, as
  picking it in the list does, in its lobby or in progress (the host puts
  the player on the smaller team).

auto.score is the score to win (0 the game type's own), auto.name and
auto.color this machine's player's name and armor color.

It logs a line at each step ("auto game: ..."), and "auto game: playing <map>"
once each game has run here for half a second, for a launcher to follow.
While a game runs the host (or a local game) logs its scores, for a launcher's
scoreboard: each player's name once a game ("auto game: player 2 is named
Sam"), and every player's score (the game type's own), kills, deaths and team
when any of them changes, at most twice a second ("auto game: scores playing to
25: player 0 s3 k3 d1 t0; player 2 s1 k1 d2 t1"), and at once as the match
ends, before its scores screen ("... scores game over to 25: ..."). Unlike
debug.network_test (network_test.c), it logs nothing else while the game
runs. (A machine logs "the host started the game: loading it" as it begins
loading a game the host started: network_client_manager.c.)

Called from the main loop every frame (main.c).
*/

#include "cseries.h"
#include "game_modes.h"
#include "main/main.h"
#include "bink/bink_playback.h"
#include "interface/player_ui.h"
#include "saved games/player_profile.h"
#include "interface/ui_widget.h"
#include "networking/network_game_globals.h"
#include "networking/network_client_manager.h"
#include "networking/network_server_manager.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "memory/data.h"
#include "game/players.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

/* the platform layer's (port/linux/src/port_config.c) */
const char *config_string(char const *name);
double config_real(char const *name);
long config_integer(char const *name);
void platform_log(char const *format, ...);

enum
{
	_auto_game_off,
	_auto_game_host,
	_auto_game_local,
	_auto_game_join,
};

static struct
{
	boolean checked;
	short mode;
	/* the level's path (levels\test\<map>\<map>, or the setting's own) */
	char map_path[128];
	char game_type[64];
	real start_delay;
	long score_to_win;
	real menu_seconds;
	boolean set_up;
	real setup_seconds;
	boolean player_in;
	boolean started;
	boolean joined;
	boolean game_over;
	real postgame_seconds;
	/* this machine's first tick of the game (NONE until it runs), and
	whether "playing" is logged */
	long first_tick;
	boolean playing;
	/* the host's scores: each player's name logged this game (by its
	absolute index), the last scores line and when it was looked at, and
	whether the match's end is logged */
	wchar_t named[HALO_PORT_MAXIMUM_NETWORK_PLAYERS][12];
	char scores_line[2048];
	long scores_tick;
	boolean over_logged;
} auto_game;

/* the launcher asked for the start (auto_game_start_now; any thread) */
static volatile int auto_game_start_asked;

/* Start the host's game now, once its player is in, when auto.start is
negative (the launcher's call: the web page's, from its thread) */
EMSCRIPTEN_KEEPALIVE void auto_game_start_now(
	void)
{
	__atomic_store_n(&auto_game_start_asked, 1, __ATOMIC_RELAXED);
}

static void auto_game_read_settings(
	void)
{
	char const *mode = config_string("auto.game");
	char const *map = config_string("auto.map");
	char const *game_type = config_string("auto.game_type");

	auto_game.checked = TRUE;
	auto_game.first_tick = NONE;
	auto_game.scores_tick = NONE;
	if (!strcmp(mode, "host"))
		auto_game.mode = _auto_game_host;
	else if (!strcmp(mode, "local"))
		auto_game.mode = _auto_game_local;
	else if (!strcmp(mode, "join"))
		auto_game.mode = _auto_game_join;
	else
	{
		if (mode[0])
			platform_log("auto game: \"%s\" is not host, local or join", mode);
		return;
	}
	if (auto_game.mode != _auto_game_join && !map[0])
	{
		platform_log("auto game: %s needs auto.map", mode);
		auto_game.mode = _auto_game_off;
		return;
	}
	/* (a level of its own path, else a multiplayer map's name) */
	if (strchr(map, '\\'))
		snprintf(auto_game.map_path, sizeof(auto_game.map_path), "%s", map);
	else
		snprintf(auto_game.map_path, sizeof(auto_game.map_path), "levels\\test\\%s\\%s", map, map);
	snprintf(auto_game.game_type, sizeof(auto_game.game_type), "%s", game_type[0] ? game_type : "slayer");
	auto_game.start_delay = (real)config_real("auto.start");
	auto_game.score_to_win = (long)config_integer("auto.score");
	if (auto_game.mode == _auto_game_join)
		platform_log("auto game: join");
	else
		platform_log("auto game: %s %s, %s", mode, map, auto_game.game_type);
}

/* this machine's first player's name (ASCII) and armor color (a multiplayer
color's name), in its profile, which it joins with; either empty keeps the
profile's (network_test.c's settings too) */
void auto_game_set_profile(
	char const *name,
	char const *color)
{
	/* the multiplayer armor colors, in the profile's order (shield_color.c) */
	static char const *const color_names[] =
	{
		"white", "black", "red", "blue", "gray", "yellow", "green", "pink", "purple",
		"cyan", "cobalt", "orange", "teal", "sage", "brown", "tan", "maroon", "salmon",
	};
	struct player_profile profile;
	short color_index = NONE;
	short index;

	for (index = 0; color && index < (short)NUMBEROF(color_names); index++)
	{
		if (!strcmp(color, color_names[index]))
			color_index = index;
	}
	if ((!name || !name[0]) && color_index == NONE)
		return;
	player_ui_get_active_player_profile(0, &profile);
	if (name && name[0])
	{
		for (index = 0; name[index] && index < MAXIMUM_PLAYER_PROFILE_NAME_LENGTH - 1; index++)
			profile.player_name[index] = (wchar_t)(unsigned char)name[index];
		profile.player_name[index] = 0;
		platform_log("auto game: this machine's player is named %s", name);
	}
	if (color_index != NONE)
	{
		profile.primary_color_index = color_index;
		platform_log("auto game: this machine's player is %s", color_names[color_index]);
	}
	player_ui_set_active_player_profile(0, player_ui_get_active_player_profile_index(0), &profile);
}

/* the game type as the menus' game settings have it, which the server's lobby
takes (game_engine_playlist_next), as picking it there does */
static void auto_game_set_game_type(
	void)
{
	struct game_variant variant;
	struct game_variant_options options;
	/* (one of OpenCE's gametypes by its name: game_modes.c) */
	boolean opence = game_modes_gametype_by_name(auto_game.game_type, &variant, &options);

	if (!opence)
		variant = *game_engine_get_variant_by_name(&variant, auto_game.game_type);
	if (auto_game.score_to_win > 0)
		variant.universal_variant.score_to_win = auto_game.score_to_win;
	player_ui_set_game_variant(&variant);
	if (opence)
		player_ui_set_game_variant_options(&options);
}

/* hosts the game: player_ui_fast_setup_network_server's, with the map and game
type set before the server's lobby is made (fast setup's empty map name is the
playlist's Derelict) */
static boolean auto_game_host(
	void)
{
	ui_widgets_close_all();
	dispose_global_network_game_server();
	dispose_global_network_game_client();
	game_connection_set(_game_connection_local);
	main_set_multiplayer_map_name(auto_game.map_path);
	auto_game_set_game_type();
	if (!ui_widget_load_by_name_or_tag(
		pc_menus_screen("ui\\shell\\main_menu\\multiplayer_type_select\\connected\\pregame\\connected_pregame_screen"),
		NONE, NULL, NONE, NONE, NONE, NONE))
	{
		platform_log("auto game: could not load the pregame screen");
		return FALSE;
	}
	game_engine_playlist_initialize();
	network_game_accept_remote_connections(TRUE);
	if (create_global_network_game_server() && create_global_network_game_client())
	{
		game_engine_playlist_begin();
		game_connection_set(_game_connection_network_server);
		return TRUE;
	}
	dispose_global_network_game_server();
	dispose_global_network_game_client();
	network_game_accept_remote_connections(FALSE);
	platform_log("auto game: could not create the game's server");
	main_goto_main_menu();
	return FALSE;
}

/* a local game: as Multiplayer > Split Screen does (split_screen_game_initialize) */
static boolean auto_game_local(
	void)
{
	ui_widgets_close_all();
	dispose_global_network_game_server();
	dispose_global_network_game_client();
	network_game_accept_remote_connections(FALSE);
	main_set_multiplayer_map_name(auto_game.map_path);
	auto_game_set_game_type();
	game_engine_playlist_initialize();
	if (create_global_network_game_server())
	{
		game_engine_playlist_begin();
		game_connection_set(_game_connection_network_server);
	}
	if (!global_network_game_client_get())
		create_global_network_game_client();
	ui_widget_load_by_name_or_tag(
		"ui\\shell\\main_menu\\multiplayer_type_select\\split_screen\\pregame\\splitscreen_pregame_wrapper_normal",
		NONE, NULL, NONE, NONE, NONE, NONE);
	return global_network_game_server_get() != NULL && global_network_game_client_get() != NULL;
}

static void auto_game_join(
	void)
{
	dispose_global_network_game_client();
	dispose_global_network_game_server();
	if (create_global_network_game_client())
	{
		game_connection_set(_game_connection_network_client);
		/* (the player joined to multiplayer first, as a player picking their
		profile: the pregame screen then asks for them every frame until they
		are in the settings) */
		player_ui_local_player_joined_multiplayer_game(0);
		platform_log("auto game: searching for games");
	}
}

/* appends to a line, cut short when it is full */
static void auto_game_append(
	char *line,
	int size,
	int *length,
	char const *format,
	...)
{
	va_list arguments;
	int written;

	if (*length >= size - 1)
		return;
	va_start(arguments, format);
	written = vsnprintf(line + *length, (size_t)(size - *length), format, arguments);
	va_end(arguments);
	if (written > 0)
		*length = MIN(*length + written, size - 1);
}

/* the host's scores, while a game runs: names new this game, then the scores
when they changed (looked at twice a second), or the match just ended */
static void auto_game_log_scores(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	boolean over = game_engine_running() && !game_engine_can_score();
	char line[sizeof(auto_game.scores_line)];
	int length = 0;

	if (!game_engine_running() || (auto_game.over_logged && over))
		return;
	if (!over && auto_game.scores_tick != NONE && game_time_get() >= auto_game.scores_tick &&
		game_time_get() - auto_game.scores_tick < TICKS_PER_SECOND / 2)
	{
		return;
	}
	auto_game.scores_tick = game_time_get();
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);

		if (absolute_index >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
			continue;
		if (csmemcmp(auto_game.named[absolute_index], player->name, sizeof(auto_game.named[absolute_index])))
		{
			char name[13];
			short index;

			csmemcpy(auto_game.named[absolute_index], player->name, sizeof(auto_game.named[absolute_index]));
			for (index = 0; index < 12 && player->name[index]; index++)
				name[index] = player->name[index] >= 32 && player->name[index] < 127 ? (char)player->name[index] : '?';
			name[index] = 0;
			platform_log("auto game: player %ld is named %s", absolute_index, name);
		}
		auto_game_append(line, (int)sizeof(line), &length, "%s player %ld s%ld k%d d%d t%ld", length ? ";" : "",
			absolute_index,
			game_engine && game_engine->get_player_score ?
				game_engine->get_player_score(iterator.datum_index, _get_score_individual) : -1L,
			player->statistics.kills[0], player->statistics.deaths, (long)player->team_index);
	}
	line[length] = 0;
	if (!over && !strcmp(line, auto_game.scores_line))
		return;
	snprintf(auto_game.scores_line, sizeof(auto_game.scores_line), "%s", line);
	auto_game.over_logged = over;
	platform_log("auto game: scores %s to %ld:%s", over ? "game over" : "playing",
		(long)game_engine_get_variant()->universal_variant.score_to_win, line);
}

/* the host's: its player in, then the start; and after a game's scores the next */
static void auto_game_update_host(
	boolean main_menu_loaded,
	real seconds)
{
	if (!main_menu_loaded)
	{
		/* the next game, once the scores have been shown a while (the host's
		button on them; a bot may press it first) */
		if (auto_game.started && game_engine_running() && !game_engine_can_score())
		{
			auto_game.game_over = TRUE;
			if (game_engine_showing_postgame() && global_network_game_server_get())
			{
				auto_game.postgame_seconds += seconds;
				if (auto_game.postgame_seconds >= 3.0f)
				{
					auto_game.postgame_seconds = 0.0f;
					network_game_server_reset_to_pregame(global_network_game_server_get());
				}
			}
		}
		return;
	}
	/* back in the lobby: started again as the first was */
	if (auto_game.game_over)
	{
		auto_game.game_over = FALSE;
		auto_game.postgame_seconds = 0.0f;
		auto_game.started = FALSE;
		auto_game.setup_seconds = 0.0f;
		__atomic_store_n(&auto_game_start_asked, 0, __ATOMIC_RELAXED);
		if (global_network_game_server_get())
		{
			/* (the scores' map choice holds the countdown, as picking the
			next game's map lets it go) */
			network_game_server_pause_countdown(global_network_game_server_get(), FALSE);
			/* a join that arrived as the last game ended would refuse every
			later one, the server holding it behind the queued player */
			network_game_server_port_clear_queued_players(global_network_game_server_get());
		}
		platform_log("auto game: the next game");
	}
	if (!auto_game.set_up)
	{
		auto_game.set_up = TRUE;
		if (auto_game.mode == _auto_game_local ? auto_game_local() : auto_game_host())
			platform_log("auto game: hosting %s", auto_game.map_path);
		else
			auto_game.mode = _auto_game_off;
		return;
	}
	if (auto_game.started || !global_network_game_client_get())
		return;
	auto_game.setup_seconds += seconds;
	/* this machine's player, as pressing A in the lobby adds one (asked again,
	every half second, until the host's settings have it) */
	if (!auto_game.player_in)
	{
		auto_game.player_in = network_game_client_port_has_local_player();
		if (auto_game.player_in)
			platform_log("auto game: this machine's player is in the lobby");
		else
			network_game_client_add_player(global_network_game_client_get(), 0);
	}
	if (auto_game.player_in && (auto_game.start_delay < 0.0f
		? __atomic_exchange_n(&auto_game_start_asked, 0, __ATOMIC_RELAXED)
		: auto_game.setup_seconds >= auto_game.start_delay))
	{
		auto_game.started = TRUE;
		network_game_client_request_immediate_start();
		platform_log("auto game: starting the game");
	}
}

static void auto_game_update_join(
	boolean main_menu_loaded)
{
	if (!main_menu_loaded)
		return;
	if (!auto_game.set_up)
	{
		auto_game.set_up = TRUE;
		auto_game_join();
	}
	else if (!auto_game.joined && network_game_client_join_first_available_game())
	{
		auto_game.joined = TRUE;
		ui_widgets_close_all();
		ui_widget_load_by_name_or_tag(
			pc_menus_screen("ui\\shell\\main_menu\\multiplayer_type_select\\connected\\pregame\\connected_pregame_screen"),
			NONE, NULL, NONE, NONE, NONE, NONE);
		platform_log("auto game: joining");
	}
}

void auto_game_update(
	boolean main_menu_loaded,
	real seconds)
{
	if (!auto_game.checked)
		auto_game_read_settings();
	if (auto_game.mode == _auto_game_off)
		return;

	/* each game's first half second here: its map loaded and the game running
	(a game joined in progress starts at the host's time) */
	if (game_in_progress() && !main_menu_loaded)
	{
		if (auto_game.first_tick == NONE || game_time_get() < auto_game.first_tick)
			auto_game.first_tick = game_time_get();
		if (!auto_game.playing && game_time_get() - auto_game.first_tick >= TICKS_PER_SECOND / 2)
		{
			auto_game.playing = TRUE;
			platform_log("auto game: playing %s", main_get_multiplayer_map_name());
		}
		if (auto_game.mode != _auto_game_join)
			auto_game_log_scores();
	}
	else if (main_menu_loaded)
	{
		auto_game.first_tick = NONE;
		auto_game.playing = FALSE;
		/* (the next game's names and scores logged afresh) */
		csmemset(auto_game.named, 0, sizeof(auto_game.named));
		auto_game.scores_line[0] = 0;
		auto_game.scores_tick = NONE;
		auto_game.over_logged = FALSE;
	}

	if (main_menu_loaded && !auto_game.set_up)
	{
		/* the intro movie skipped first, as a button press does: the movie
		holds the texture cache's spare memory, which starting the map takes
		too (texture_cache_steal_memory's assertion) */
		if (bink_playback_in_progress())
		{
			bink_playback_stop();
			auto_game.menu_seconds = 0.0f;
			return;
		}
		/* (the main menu settling first) */
		if (auto_game.menu_seconds < 2.0f)
		{
			auto_game.menu_seconds += seconds;
			return;
		}
		/* (the profile's name and color, before the player joins with them) */
		auto_game_set_profile(config_string("auto.name"), config_string("auto.color"));
	}
	if (auto_game.mode == _auto_game_join)
		auto_game_update_join(main_menu_loaded);
	else
		auto_game_update_host(main_menu_loaded, seconds);
}
