/* canvas-wl runtime configuration (TOML-driven).
 *
 * No compile-time settings live here anymore. This header only declares the
 * config struct + loader. Defaults live in config.c (mirroring the old
 * suckless config.h values) and the user file lives at
 *   ~/.config/canvaswl/canvaswl.toml
 * (or $XDG_CONFIG_HOME/canvaswl/canvaswl.toml, or --config <path>).
 *
 * If the file is missing it is auto-generated with defaults, so you never
 * have to mkdir + nano it yourself.
 */

#ifndef CANVASWL_CONFIG_H
#define CANVASWL_CONFIG_H

#include <stddef.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <wlr/types/wlr_keyboard.h>

/* X11-style modifier aliases (kept so old config lines map cleanly). */
#ifndef Mod4Mask
#define Mod4Mask   WLR_MODIFIER_LOGO
#define Mod1Mask   WLR_MODIFIER_ALT
#define ShiftMask  WLR_MODIFIER_SHIFT
#define ControlMask WLR_MODIFIER_CTRL
#endif

/* Dynamic keybinding table types. */
typedef enum {
    action_run_command,          /* fork + run command_to_run via /bin/sh */
    action_close_focused_window, /* close the focused window */
    action_quit_window_manager,  /* exit the compositor */
    action_focus_next_window,    /* smoothly pan camera to next window */
    action_focus_previous_window,/* smoothly pan camera to previous window */
    action_reload_config         /* re-read the TOML file at runtime */
} KeyActionType;

typedef struct {
    unsigned int modifier_mask;   /* e.g. Mod4Mask, Mod4Mask | ShiftMask */
    xkb_keysym_t key_symbol;      /* e.g. XKB_KEY_Return, XKB_KEY_Q */
    KeyActionType action_type;    /* what to do when pressed */
    char *command_to_run;         /* owned; only for action_run_command */
} KeyBinding;

typedef struct {
    /* [window] */
    int window_border_width;
    unsigned long window_border_color_active;
    unsigned long window_border_color_inactive;
    int default_window_width;
    int default_window_height;
    int default_window_x;
    int default_window_y;
    int minimum_window_width;
    int minimum_window_height;

    /* [focus] */
    int window_focus_cycle_uses_recent_order; /* 0 = nearest, 1 = MRU */
    int window_focus_animation_duration_ms;
    int window_focus_animation_frames_per_second; /* parsed, currently unused */
    int window_focus_animation_interruption_mode; /* 0 ignore / 1 coalesce / 2 retarget */

    /* [input] */
    unsigned int main_modifier;
    unsigned int resize_modifier;
    unsigned int mouse_button_focus;
    unsigned int mouse_button_move;
    unsigned int mouse_button_camera;
    unsigned int mouse_button_resize;

    /* [cursor] */
    char *cursor_theme_name; /* owned, NULL = default Xcursor theme */
    int cursor_size;

    /* [[keybind]] */
    KeyBinding *keybindings; /* owned array */
    unsigned int keybinding_count;

    /* [startup] */
    char **startup_commands; /* owned array of owned strings */
    unsigned int startup_command_count;
} canvas_config;

/* Fill *cfg with compiled-in defaults (the old config.h values). */
void config_init_defaults(canvas_config *cfg);

/* Load file at path into *cfg (starts from defaults, overrides from TOML).
 * Returns 0 on success, -1 on error with errbuf filled. */
int config_load(const char *path, canvas_config *cfg, char *errbuf, size_t errbufsz);

/* Free heap members of *cfg (safe on defaults). Does not free cfg itself. */
void config_free_contents(canvas_config *cfg);

/* Resolve the default config path:
 *   --config flag value (if non-NULL) else
 *   $XDG_CONFIG_HOME/canvaswl/canvaswl.toml else
 *   ~/.config/canvaswl/canvaswl.toml
 * Returns 0 on success. */
int config_default_path(char *buf, size_t bufsz);

/* Write a commented default config file to path (mkdir -p's the parent).
 * The contents are copied from the shipped config.toml.def template
 * (see find order in config.c: $CANVASWL_TEMPLATE, next to the binary,
 * $PREFIX/share/canvaswl/, /usr/local/share, /usr/share, ./).
 * Returns 0 on success, -1 on error with errbuf filled. */
int config_write_default_file(const char *path, char *errbuf, size_t errbufsz);

/* Name helpers (used by config.c internally, exposed for error messages).
 * NOTE: these are silent (no stderr); callers report errors with context.
 * config_parse_modifier_mask accepts "", "none"/"0" as no modifier (0). */
int config_parse_modifier_mask(const char *s, unsigned int *out);
int config_parse_mouse_button(const char *s, unsigned int *out);
int config_parse_color(const char *s, unsigned long *out);
int config_parse_action(const char *s, KeyActionType *out);

/* Validate file at path without applying it (`canvas validate`).
 * Fills report (multi-line, human readable) and returns:
 *   0  = valid (report holds "OK ..." summary),
 *   >0 = number of problems found (report lists them),
 *   -1 = fatal (cannot open / TOML syntax error; report holds reason).
 * Keybinds may omit mod/modifier or set mod = "" / "none" for no modifier. */
int config_validate(const char *path, char *report, size_t reportsz);

#endif /* CANVASWL_CONFIG_H */
