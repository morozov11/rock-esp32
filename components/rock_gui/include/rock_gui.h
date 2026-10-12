#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROCK_GUI_MAX_STATIONS 20

// Numeric mirror of rock_player_status_t (player_bsp.h). The Rust binding
// passes this field as a raw int (platform.rs rock_gui_update_playback), so
// the values must stay in exact lockstep until the phase-5 shared ABI header.
typedef enum {
    ROCK_GUI_PLAYBACK_IDLE = 0,
    ROCK_GUI_PLAYBACK_BUFFERING,
    ROCK_GUI_PLAYBACK_PLAYING,
    ROCK_GUI_PLAYBACK_PAUSED,
    ROCK_GUI_PLAYBACK_STOPPED,
    ROCK_GUI_PLAYBACK_ERROR
} rock_gui_playback_t;

typedef struct {
    char id[129];
    char name[129];
    char tags[64];
    char codec[16];
    uint32_t bitrate_kbps;
    bool is_playing;
} rock_gui_station_item_t;

typedef enum {
    ROCK_UI_EVT_NONE = 0,
    ROCK_UI_EVT_PLAY_STATION,   // text: station_id
    ROCK_UI_EVT_PLAYBACK_PLAY,
    ROCK_UI_EVT_PLAYBACK_PAUSE,
    ROCK_UI_EVT_PLAYBACK_STOP,
    ROCK_UI_EVT_VOLUME_CHANGE,  // value: -5 / +5
    ROCK_UI_EVT_CATALOG_BROWSE, // text: optional cursor
    ROCK_UI_EVT_CATALOG_SEARCH, // text: query (<=128 chars)
    ROCK_UI_EVT_RETRY,
    ROCK_UI_EVT_CAST_BROWSE,
    ROCK_UI_EVT_CAST_SELECT,    // text: receiver_id
    ROCK_UI_EVT_CAST_DISCONNECT,
} rock_ui_event_type_t;

#define ROCK_GUI_MAX_CAST_DEVICES 16

typedef struct {
    char receiver_id[65];
    char display_name[129];
    bool is_active;
} rock_gui_cast_item_t;

typedef struct {
    rock_ui_event_type_t type;
    char text[513];
    int16_t value;
} rock_ui_event_t;

/**
 * Initializes and displays the native RockCast radio GUI on the ST7701 display.
 * Creates the Shell (Header, Viewport, Ticker), initializes Now-Playing,
 * Browse, and Search screens, and starts the real spectrum FFT analysis task.
 */
int rock_gui_start(void);
void rock_gui_present(const char *station_id, const char *title, const char *subtitle);

/**
 * Non-blocking poll for an event emitted by touch interaction from the UI queue.
 * Returns true if an event was retrieved within timeout_ms.
 */
bool rock_gui_poll_event(rock_ui_event_t *out_event, uint32_t timeout_ms);

/**
 * Update playback state on the GUI (Now-Playing view, status ticker, transport buttons).
 */
void rock_gui_update_playback(rock_gui_playback_t status, const char *station_id,
                              const char *station_name,
                              const char *title, const char *subtitle,
                              uint8_t volume, bool muted);

/**
 * Update network and server connectivity indicators in the top bar.
 */
void rock_gui_update_connectivity(int rssi, bool wifi_connected, bool server_connected);

/**
 * Set stations list for the Browse screen (replaces existing page with up to 20 stations).
 */
void rock_gui_set_browse_stations(const rock_gui_station_item_t *items, size_t count,
                                  const char *next_cursor);

/**
 * Set search results list (transitions to list display with search results).
 */
void rock_gui_set_search_results(const rock_gui_station_item_t *items, size_t count);

/**
 * Show state overlay screens (loading, empty, offline, error).
 */
void rock_gui_show_loading(const char *message);
void rock_gui_show_empty(const char *message);
void rock_gui_show_offline(const char *message);
void rock_gui_show_error(const char *message);

/**
 * Return to previous view from an overlay.
 */
void rock_gui_dismiss_state(void);

/**
 * Mark catalog data as stale/hidden when server connection is lost.
 */
void rock_gui_mark_catalog_stale(void);

/**
 * Notify GUI that a station play command was accepted or initiated.
 */
void rock_gui_set_starting_station(const char *station_id);

/**
 * Measure GUI performance: returns approximate FPS and memory stats.
 */
void rock_gui_get_metrics(uint32_t *out_fps, size_t *out_free_heap);

/**
 * Check if the native station GUI is currently running.
 */
bool rock_gui_is_active(void);

/**
 * Switch active UI view programmatically:
 * 0 = Now Playing, 1 = Browse, 2 = Search
 */
void rock_gui_switch_view(int view_index);

/**
 * Execute a serial-console UI action against the live widgets (under the
 * LVGL lock). Action strings are the CMD:UI: payload of the serial protocol
 * (CATALOG, SEARCH, SEARCH:<q>, PLAYPAUSE, STOP, VOL+, VOL-, RETRY, BACK,
 * STATION:<row>, STATIONID:<id>, CAST, CAST:SELECT:<row>, CAST:LOCAL).
 */
void rock_gui_trigger(const char *action);

/**
 * Log one diagnostics line with the GUI's internal view/catalog state
 * (format parsed by tools/rock_hwtest.py — keep verbatim).
 */
void rock_gui_log_debug_state(void);

/**
 * Show audio output selection dialog / menu.
 */
void rock_gui_show_cast_menu(bool scanning);

/**
 * Update the list of discovered Chromecast devices in the cast menu.
 */
void rock_gui_set_cast_devices(const rock_gui_cast_item_t *items, size_t count);

/**
 * Close/dismiss the cast menu.
 */
void rock_gui_dismiss_cast_menu(void);

/**
 * Update cast badge and button state on Now-Playing.
 */
void rock_gui_update_cast_status(bool is_casting, const char *receiver_name);

#ifdef __cplusplus
}
#endif
