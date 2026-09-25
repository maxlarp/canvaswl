/* canvas-wl runtime TOML config. See config.h. */

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "toml.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

/* ------------------------------------------------------------------ */
/* defaults (the old suckless config.h values)                         */
/* ------------------------------------------------------------------ */

static int str_is_empty(const char *s) {
    return !s || !s[0];
}

void config_init_defaults(canvas_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->window_border_width = 2;
    cfg->window_border_color_active = 0x00AAFF;
    cfg->window_border_color_inactive = 0x444444;
    cfg->default_window_width = 800;
    cfg->default_window_height = 600;
    cfg->default_window_x = 0;
    cfg->default_window_y = 0;
    cfg->minimum_window_width = 1;
    cfg->minimum_window_height = 1;

    cfg->window_focus_cycle_uses_recent_order = 1;
    cfg->window_focus_animation_duration_ms = 250;
    cfg->window_focus_animation_frames_per_second = 60;
    cfg->window_focus_animation_interruption_mode = 1;

    cfg->main_modifier = Mod4Mask;
    cfg->resize_modifier = Mod1Mask;
    cfg->mouse_button_focus = BTN_LEFT;
    cfg->mouse_button_move = BTN_LEFT;
    cfg->mouse_button_camera = BTN_RIGHT;
    cfg->mouse_button_resize = BTN_RIGHT;

    cfg->cursor_theme_name = NULL;
    cfg->cursor_size = 24;

    cfg->keybinding_count = 6;
    cfg->keybindings = calloc(cfg->keybinding_count, sizeof(KeyBinding));
    if (cfg->keybindings) {
        cfg->keybindings[0] = (KeyBinding){ Mod4Mask, XKB_KEY_Return, action_run_command, NULL };
        cfg->keybindings[1] = (KeyBinding){ Mod4Mask, XKB_KEY_D, action_run_command, NULL };
        cfg->keybindings[2] = (KeyBinding){ Mod4Mask, XKB_KEY_Q, action_close_focused_window, NULL };
        cfg->keybindings[3] = (KeyBinding){ Mod4Mask, XKB_KEY_Tab, action_focus_next_window, NULL };
        cfg->keybindings[4] = (KeyBinding){ Mod4Mask | ShiftMask, XKB_KEY_Tab, action_focus_previous_window, NULL };
        cfg->keybindings[5] = (KeyBinding){ Mod4Mask | ShiftMask, XKB_KEY_E, action_quit_window_manager, NULL };
        /* strdup can fail; NULL cmd is handled gracefully at runtime. */
        cfg->keybindings[0].command_to_run = strdup("alacritty");
        cfg->keybindings[1].command_to_run = strdup("fuzzel");
    } else {
        cfg->keybinding_count = 0;
    }

    cfg->startup_command_count = 1;
    cfg->startup_commands = calloc(1, sizeof(char *));
    if (cfg->startup_commands) {
        cfg->startup_commands[0] = strdup("swaybg -i ~/wallpaper.jpg &");
        if (!cfg->startup_commands[0]) {
            free(cfg->startup_commands);
            cfg->startup_commands = NULL;
            cfg->startup_command_count = 0;
        }
    } else {
        cfg->startup_command_count = 0;
    }
}

void config_free_contents(canvas_config *cfg) {
    if (!cfg) {
        return;
    }
    if (cfg->keybindings) {
        for (unsigned int i = 0; i < cfg->keybinding_count; i++) {
            free(cfg->keybindings[i].command_to_run);
        }
        free(cfg->keybindings);
        cfg->keybindings = NULL;
        cfg->keybinding_count = 0;
    }
    if (cfg->startup_commands) {
        for (unsigned int i = 0; i < cfg->startup_command_count; i++) {
            free(cfg->startup_commands[i]);
        }
        free(cfg->startup_commands);
        cfg->startup_commands = NULL;
        cfg->startup_command_count = 0;
    }
    free(cfg->cursor_theme_name);
    cfg->cursor_theme_name = NULL;
}

/* ------------------------------------------------------------------ */
/* name parsers                                                        */
/* ------------------------------------------------------------------ */

static void str_tolower_inplace(char *s) {
    for (; *s; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

int config_parse_modifier_mask(const char *s, unsigned int *out) {
    if (!out) {
        return -1;
    }
    if (str_is_empty(s)) {
        *out = 0;
        return 0;
    }
    /* Bare integer fallback. */
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end && *end == '\0') {
        *out = (unsigned int)v;
        return 0;
    }

    unsigned int mask = 0;
    /* Split on '+', '|', ',', ' ', '\t'. */
    char *copy = strdup(s);
    if (!copy) {
        return -1;
    }
    int any = 0;
    char *tok = strtok(copy, "+|, \t");
    while (tok) {
        char *t = strdup(tok);
        if (!t) {
            free(copy);
            return -1;
        }
        str_tolower_inplace(t);
        if (!strcmp(t, "shift") || !strcmp(t, "shiftmask")) {
            mask |= ShiftMask;
        } else if (!strcmp(t, "ctrl") || !strcmp(t, "control") ||
                !strcmp(t, "controlmask")) {
            mask |= ControlMask;
        } else if (!strcmp(t, "alt") || !strcmp(t, "mod1") ||
                !strcmp(t, "mod1mask")) {
            mask |= Mod1Mask;
        } else if (!strcmp(t, "super") || !strcmp(t, "logo") ||
                !strcmp(t, "windows") || !strcmp(t, "win") ||
                !strcmp(t, "mod4") || !strcmp(t, "mod4mask")) {
            mask |= Mod4Mask;
        } else if (!strcmp(t, "none") || !strcmp(t, "0")) {
            /* no bits */
        } else {
            free(t);
            free(copy);
            return -1;
        }
        any = 1;
        free(t);
        tok = strtok(NULL, "+|, \t");
    }
    free(copy);
    if (!any) {
        return -1;
    }
    *out = mask;
    return 0;
}

int config_parse_mouse_button(const char *s, unsigned int *out) {
    if (!out || str_is_empty(s)) {
        return -1;
    }
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end && *end == '\0') {
        *out = (unsigned int)v;
        return 0;
    }
    char *t = strdup(s);
    if (!t) {
        return -1;
    }
    str_tolower_inplace(t);
    /* Strip optional btn_ prefix. */
    const char *n = t;
    if (!strncmp(n, "btn_", 4)) {
        n += 4;
    }
    unsigned int code = 0;
    if (!strcmp(n, "left")) {
        code = BTN_LEFT;
    } else if (!strcmp(n, "right")) {
        code = BTN_RIGHT;
    } else if (!strcmp(n, "middle")) {
        code = BTN_MIDDLE;
    } else if (!strcmp(n, "side")) {
        code = BTN_SIDE;
    } else if (!strcmp(n, "extra")) {
        code = BTN_EXTRA;
    } else if (!strcmp(n, "forward")) {
        code = BTN_FORWARD;
    } else if (!strcmp(n, "back")) {
        code = BTN_BACK;
    } else if (!strcmp(n, "task")) {
        code = BTN_TASK;
    } else {
        free(t);
        return -1;
    }
    free(t);
    *out = code;
    return 0;
}

int config_parse_color(const char *s, unsigned long *out) {
    if (!out || str_is_empty(s)) {
        return -1;
    }
    /* Skip # prefix. */
    const char *p = s;
    if (*p == '#') {
        p++;
    }
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 0);
    if (end && *end == '\0') {
        /* strtoul with base 0 handles 0x.. and decimal, but NOT bare hex
         * like "00AAFF". Retry as hex if the 0-base parse consumed it as
         * decimal-looking or failed. */
        int is_hex_word = 1;
        for (const char *q = p; *q; q++) {
            if (!isxdigit((unsigned char)*q)) {
                is_hex_word = 0;
                break;
            }
        }
        if (*p && is_hex_word && v > 0) {
            /* Ambiguous (e.g. "444444" is valid decimal too). Prefer hex
             * when the string contains a-f/A-F, else keep base-0 result. */
            int has_alpha = 0;
            for (const char *q = p; *q; q++) {
                if ((*q >= 'a' && *q <= 'f') || (*q >= 'A' && *q <= 'F')) {
                    has_alpha = 1;
                    break;
                }
            }
            if (has_alpha) {
                v = strtoul(p, NULL, 16);
            } else if (!strncmp(p, "0x", 2) || !strncmp(p, "0X", 2)) {
                /* already handled by base 0 */
            } else {
                /* All-digit color like "444444": author almost surely means
                 * hex. TOML ints can't express that (they'd be decimal), so
                 * treat all-digit 6-char strings as hex. */
                size_t len = strlen(p);
                if (len == 6 || len == 8) {
                    v = strtoul(p, NULL, 16);
                }
            }
        }
        *out = v & 0xFFFFFF;
        return 0;
    }
    /* Last resort: pure hex word without prefix. */
    v = strtoul(p, &end, 16);
    if (end && *end == '\0') {
        *out = v & 0xFFFFFF;
        return 0;
    }
    return -1;
}

int config_parse_action(const char *s, KeyActionType *out) {
    if (!out || str_is_empty(s)) {
        return -1;
    }
    char *t = strdup(s);
    if (!t) {
        return -1;
    }
    str_tolower_inplace(t);
    KeyActionType a;
    if (!strcmp(t, "spawn") || !strcmp(t, "run") ||
            !strcmp(t, "run_command") || !strcmp(t, "action_run_command") ||
            !strcmp(t, "exec")) {
        a = action_run_command;
    } else if (!strcmp(t, "close") || !strcmp(t, "close_focused_window") ||
            !strcmp(t, "action_close_focused_window") || !strcmp(t, "kill")) {
        a = action_close_focused_window;
    } else if (!strcmp(t, "quit") || !strcmp(t, "exit") ||
            !strcmp(t, "logout") || !strcmp(t, "quit_window_manager") ||
            !strcmp(t, "action_quit_window_manager")) {
        a = action_quit_window_manager;
    } else if (!strcmp(t, "focus_next") || !strcmp(t, "next") ||
            !strcmp(t, "focus_next_window") ||
            !strcmp(t, "action_focus_next_window")) {
        a = action_focus_next_window;
    } else if (!strcmp(t, "focus_prev") || !strcmp(t, "prev") ||
            !strcmp(t, "previous") || !strcmp(t, "focus_previous") ||
            !strcmp(t, "focus_previous_window") ||
            !strcmp(t, "action_focus_previous_window")) {
        a = action_focus_previous_window;
    } else if (!strcmp(t, "reload") || !strcmp(t, "reload_config") ||
            !strcmp(t, "action_reload_config") || !strcmp(t, "refresh")) {
        a = action_reload_config;
    } else {
        free(t);
        return -1;
    }
    free(t);
    *out = a;
    return 0;
}

static xkb_keysym_t parse_keysym(const char *s) {
    if (str_is_empty(s)) {
        return XKB_KEY_NoSymbol;
    }
    /* Bare integer fallback (keysym value). */
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end && *end == '\0' && v != 0) {
        return (xkb_keysym_t)v;
    }
    xkb_keysym_t sym = xkb_keysym_from_name(s, XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym != XKB_KEY_NoSymbol) {
        return sym;
    }
    /* Allow "XKB_KEY_Return" spelling. */
    const char *prefix = "XKB_KEY_";
    if (!strncmp(s, prefix, strlen(prefix))) {
        sym = xkb_keysym_from_name(s + strlen(prefix),
            XKB_KEYSYM_CASE_INSENSITIVE);
    }
    return sym;
}

/* ------------------------------------------------------------------ */
/* toml field helpers (int-or-string tolerant)                          */
/* ------------------------------------------------------------------ */

static int get_int(toml_table_t *tab, const char *key, int64_t *out) {
    toml_datum_t d = toml_int_in(tab, key);
    if (d.ok) {
        *out = d.u.i;
        return 0;
    }
    toml_datum_t b = toml_bool_in(tab, key);
    if (b.ok) {
        *out = b.u.b ? 1 : 0;
        return 0;
    }
    return -1;
}

static int get_bool(toml_table_t *tab, const char *key, int *out) {
    toml_datum_t b = toml_bool_in(tab, key);
    if (b.ok) {
        *out = b.u.b ? 1 : 0;
        return 0;
    }
    int64_t i;
    if (get_int(tab, key, &i) == 0) {
        *out = i ? 1 : 0;
        return 0;
    }
    return -1;
}

/* String field: caller must free *out. Returns 0 if present. */
static int get_string(toml_table_t *tab, const char *key, char **out) {
    toml_datum_t d = toml_string_in(tab, key);
    if (d.ok) {
        *out = d.u.s;
        return 0;
    }
    return -1;
}

static int get_mods(toml_table_t *tab, const char *key, unsigned int *out) {
    char *s = NULL;
    if (get_string(tab, key, &s) == 0) {
        int rc = config_parse_modifier_mask(s, out);
        free(s);
        return rc;
    }
    int64_t i;
    if (get_int(tab, key, &i) == 0) {
        *out = (unsigned int)i;
        return 0;
    }
    return -1;
}

static int get_button(toml_table_t *tab, const char *key, unsigned int *out) {
    char *s = NULL;
    if (get_string(tab, key, &s) == 0) {
        int rc = config_parse_mouse_button(s, out);
        free(s);
        return rc;
    }
    int64_t i;
    if (get_int(tab, key, &i) == 0) {
        *out = (unsigned int)i;
        return 0;
    }
    return -1;
}

static int get_color(toml_table_t *tab, const char *key, unsigned long *out) {
    int64_t i;
    if (get_int(tab, key, &i) == 0) {
        *out = (unsigned long)(i & 0xFFFFFF);
        return 0;
    }
    char *s = NULL;
    if (get_string(tab, key, &s) == 0) {
        int rc = config_parse_color(s, out);
        free(s);
        return rc;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* main loader                                                         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* diagnostics for `canvas validate` (and runtime stderr warnings)   */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int issues;
} config_diag;

static void diag_add(config_diag *d, const char *fmt, ...) {
    if (!d) {
        return;
    }
    d->issues++;
    if (!d->buf || d->cap == 0) {
        return;
    }
    if (d->len >= d->cap - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(d->buf + d->len, d->cap - d->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= d->cap - d->len) {
        d->len = d->cap - 1;
    } else {
        d->len += (size_t)n;
    }
}

/* Describe the raw value of a key for error messages. Best effort:
 * prefers a quoted string, then int/bool, then raw text. */
static void diag_value_suffix(toml_table_t *tab, const char *key, char *out,
        size_t outsz) {
    toml_datum_t s = toml_string_in(tab, key);
    if (s.ok) {
        snprintf(out, outsz, " (got \"%s\")", s.u.s);
        free(s.u.s);
        return;
    }
    toml_datum_t i = toml_int_in(tab, key);
    if (i.ok) {
        snprintf(out, outsz, " (got %lld)", (long long)i.u.i);
        return;
    }
    toml_datum_t b = toml_bool_in(tab, key);
    if (b.ok) {
        snprintf(out, outsz, " (got %s)", b.u.b ? "true" : "false");
        return;
    }
    toml_raw_t r = toml_raw_in(tab, key);
    if (r) {
        snprintf(out, outsz, " (got %s)", r);
        return;
    }
    out[0] = '\0';
}

static int key_present(toml_table_t *t, const char *key) {
    return t && key && toml_key_exists(t, key);
}

int config_load(const char *path, canvas_config *cfg, char *errbuf,
        size_t errbufsz) {
    if (!path || !cfg) {
        if (errbuf && errbufsz) {
            snprintf(errbuf, errbufsz, "no path or config");
        }
        return -1;
    }
    config_init_defaults(cfg);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        snprintf(errbuf, errbufsz, "cannot open %s: %s", path,
            strerror(errno));
        return -1;
    }
    char toml_err[512] = {0};
    toml_table_t *root = toml_parse_file(fp, toml_err, sizeof(toml_err));
    fclose(fp);
    if (!root) {
        snprintf(errbuf, errbufsz, "%s: %s", path,
            toml_err[0] ? toml_err : "toml parse error");
        return -1;
    }

    int64_t iv;
    int bv;
    unsigned long cv;
    unsigned int mv;
    char *sv = NULL;

    toml_table_t *t = toml_table_in(root, "window");
    if (t) {
        if (get_int(t, "border_width", &iv) == 0) {
            if (iv < 0) {
                fprintf(stderr, "canvas-wl: [window] border_width < 0, keeping default\n");
            } else {
                cfg->window_border_width = (int)iv;
            }
        } else if (key_present(t, "border_width")) {
            char vs[128] = {0};
            diag_value_suffix(t, "border_width", vs, sizeof(vs));
            fprintf(stderr, "canvas-wl: [window] border_width: expected integer >= 0%s, keeping default\n", vs);
        }
        if (get_color(t, "border_active", &cv) == 0) {
            cfg->window_border_color_active = cv;
        } else if (key_present(t, "border_active")) {
            char vs[128] = {0};
            diag_value_suffix(t, "border_active", vs, sizeof(vs));
            fprintf(stderr, "canvas-wl: [window] border_active: bad color%s, keeping default\n", vs);
        }
        if (get_color(t, "border_inactive", &cv) == 0) {
            cfg->window_border_color_inactive = cv;
        } else if (key_present(t, "border_inactive")) {
            char vs[128] = {0};
            diag_value_suffix(t, "border_inactive", vs, sizeof(vs));
            fprintf(stderr, "canvas-wl: [window] border_inactive: bad color%s, keeping default\n", vs);
        }
        if (get_int(t, "default_width", &iv) == 0) {
            if (iv <= 0) {
                fprintf(stderr, "canvas-wl: [window] default_width must be > 0, keeping default\n");
            } else {
                cfg->default_window_width = (int)iv;
            }
        } else if (key_present(t, "default_width")) {
            char vs[128] = {0};
            diag_value_suffix(t, "default_width", vs, sizeof(vs));
            fprintf(stderr, "canvas-wl: [window] default_width: expected integer > 0%s, keeping default\n", vs);
        }
        if (get_int(t, "default_height", &iv) == 0) {
            if (iv <= 0) {
                fprintf(stderr, "canvas-wl: [window] default_height must be > 0, keeping default\n");
            } else {
                cfg->default_window_height = (int)iv;
            }
        } else if (key_present(t, "default_height")) {
            char vs[128] = {0};
            diag_value_suffix(t, "default_height", vs, sizeof(vs));
            fprintf(stderr, "canvas-wl: [window] default_height: expected integer > 0%s, keeping default\n", vs);
        }
        if (get_int(t, "offset_x", &iv) == 0) {
            cfg->default_window_x = (int)iv;
        } else if (key_present(t, "offset_x")) {
            fprintf(stderr, "canvas-wl: [window] offset_x: expected integer, keeping default\n");
        }
        if (get_int(t, "offset_y", &iv) == 0) {
            cfg->default_window_y = (int)iv;
        } else if (key_present(t, "offset_y")) {
            fprintf(stderr, "canvas-wl: [window] offset_y: expected integer, keeping default\n");
        }
        if (get_int(t, "min_width", &iv) == 0) {
            if (iv <= 0) {
                fprintf(stderr, "canvas-wl: [window] min_width must be > 0, keeping default\n");
            } else {
                cfg->minimum_window_width = (int)iv;
            }
        } else if (key_present(t, "min_width")) {
            char vs[128] = {0};
            diag_value_suffix(t, "min_width", vs, sizeof(vs));
            fprintf(stderr, "canvas-wl: [window] min_width: expected integer > 0%s, keeping default\n", vs);
        }
        if (get_int(t, "min_height", &iv) == 0) {
            if (iv <= 0) {
                fprintf(stderr, "canvas-wl: [window] min_height must be > 0, keeping default\n");
            } else {
                cfg->minimum_window_height = (int)iv;
            }
        } else if (key_present(t, "min_height")) {
            char vs[128] = {0};
            diag_value_suffix(t, "min_height", vs, sizeof(vs));
            fprintf(stderr, "canvas-wl: [window] min_height: expected integer > 0%s, keeping default\n", vs);
        }
    }

    t = toml_table_in(root, "focus");
    if (t) {
        if (get_bool(t, "recent_order", &bv) == 0) {
            cfg->window_focus_cycle_uses_recent_order = bv;
        } else if (key_present(t, "recent_order")) {
            fprintf(stderr, "canvas-wl: [focus] recent_order: expected bool, keeping default\n");
        }
        if (get_int(t, "anim_duration_ms", &iv) == 0) {
            if (iv < 0) {
                fprintf(stderr, "canvas-wl: [focus] anim_duration_ms < 0, keeping default\n");
            } else {
                cfg->window_focus_animation_duration_ms = (int)iv;
            }
        } else if (key_present(t, "anim_duration_ms")) {
            fprintf(stderr, "canvas-wl: [focus] anim_duration_ms: expected integer >= 0, keeping default\n");
        }
        if (get_int(t, "anim_fps", &iv) == 0) {
            if (iv <= 0) {
                fprintf(stderr, "canvas-wl: [focus] anim_fps must be > 0, keeping default\n");
            } else {
                cfg->window_focus_animation_frames_per_second = (int)iv;
            }
        } else if (key_present(t, "anim_fps")) {
            fprintf(stderr, "canvas-wl: [focus] anim_fps: expected integer > 0, keeping default\n");
        }
        if (get_int(t, "interruption_mode", &iv) == 0) {
            if (iv < 0 || iv > 2) {
                fprintf(stderr, "canvas-wl: [focus] interruption_mode must be 0..2, keeping default\n");
            } else {
                cfg->window_focus_animation_interruption_mode = (int)iv;
            }
        } else if (key_present(t, "interruption_mode")) {
            fprintf(stderr, "canvas-wl: [focus] interruption_mode: expected integer 0..2, keeping default\n");
        }
    }

    t = toml_table_in(root, "input");
    if (t) {
        if (get_mods(t, "main_mod", &mv) == 0) {
            cfg->main_modifier = mv;
        } else if (key_present(t, "main_mod")) {
            char vs[128] = {0};
            diag_value_suffix(t, "main_mod", vs, sizeof(vs));
            fprintf(stderr, "canvas-wl: [input] main_mod: unknown modifier%s, keeping default\n", vs);
        }
        if (get_mods(t, "resize_mod", &mv) == 0) {
            cfg->resize_modifier = mv;
        } else if (key_present(t, "resize_mod")) {
            char vs[128] = {0};
            diag_value_suffix(t, "resize_mod", vs, sizeof(vs));
            fprintf(stderr, "canvas-wl: [input] resize_mod: unknown modifier%s, keeping default\n", vs);
        }
        if (get_button(t, "btn_focus", &mv) == 0) {
            cfg->mouse_button_focus = mv;
        } else if (key_present(t, "btn_focus")) {
            char vs[128] = {0};
            diag_value_suffix(t, "btn_focus", vs, sizeof(vs));
            fprintf(stderr, "canvas-wl: [input] btn_focus: unknown mouse button%s, keeping default\n", vs);
        }
        if (get_button(t, "btn_move", &mv) == 0) {
            cfg->mouse_button_move = mv;
        } else if (key_present(t, "btn_move")) {
            char vs[128] = {0};
            diag_value_suffix(t, "btn_move", vs, sizeof(vs));
            fprintf(stderr, "canvas-wl: [input] btn_move: unknown mouse button%s, keeping default\n", vs);
        }
        if (get_button(t, "btn_camera", &mv) == 0) {
            cfg->mouse_button_camera = mv;
        } else if (key_present(t, "btn_camera")) {
            char vs[128] = {0};
            diag_value_suffix(t, "btn_camera", vs, sizeof(vs));
            fprintf(stderr, "canvas-wl: [input] btn_camera: unknown mouse button%s, keeping default\n", vs);
        }
        if (get_button(t, "btn_resize", &mv) == 0) {
            cfg->mouse_button_resize = mv;
        } else if (key_present(t, "btn_resize")) {
            char vs[128] = {0};
            diag_value_suffix(t, "btn_resize", vs, sizeof(vs));
            fprintf(stderr, "canvas-wl: [input] btn_resize: unknown mouse button%s, keeping default\n", vs);
        }
    }

    t = toml_table_in(root, "cursor");
    if (t) {
        if (get_string(t, "theme", &sv) == 0) {
            free(cfg->cursor_theme_name);
            /* Empty string = default theme (NULL). */
            cfg->cursor_theme_name = sv[0] ? sv : NULL;
            if (!sv[0]) {
                free(sv);
            }
        } else if (key_present(t, "theme")) {
            fprintf(stderr, "canvas-wl: [cursor] theme: expected string, keeping default\n");
        }
        if (get_int(t, "size", &iv) == 0) {
            if (iv <= 0) {
                fprintf(stderr, "canvas-wl: [cursor] size must be > 0, keeping default\n");
            } else {
                cfg->cursor_size = (int)iv;
            }
        } else if (key_present(t, "size")) {
            fprintf(stderr, "canvas-wl: [cursor] size: expected integer > 0, keeping default\n");
        }
    }

    toml_array_t *kb = toml_array_in(root, "keybind");
    if (kb) {
        int n = toml_array_nelem(kb);
        KeyBinding *arr = NULL;
        unsigned int count = 0;
        if (n > 0) {
            arr = calloc((size_t)n, sizeof(KeyBinding));
            if (!arr) {
                snprintf(errbuf, errbufsz, "out of memory");
                toml_free(root);
                return -1;
            }
        }
        for (int i = 0; i < n; i++) {
            toml_table_t *e = toml_table_at(kb, i);
            if (!e) {
                fprintf(stderr, "canvas-wl: keybind #%d: not a table, skipping\n", i);
                continue;
            }
            /* mod is optional: missing/empty/"none" means no modifier. */
            unsigned int mods = 0;
            int have_mods = (get_mods(e, "mod", &mods) == 0);
            if (!have_mods) {
                /* Back-compat alias. */
                have_mods = (get_mods(e, "modifier", &mods) == 0);
            }
            if (!have_mods) {
                if (!key_present(e, "mod") && !key_present(e, "modifier")) {
                    mods = 0;
                    have_mods = 1;
                } else {
                    char vs[128] = {0};
                    if (key_present(e, "mod")) {
                        diag_value_suffix(e, "mod", vs, sizeof(vs));
                    } else {
                        diag_value_suffix(e, "modifier", vs, sizeof(vs));
                    }
                    fprintf(stderr, "canvas-wl: keybind #%d: bad mod%s, skipping\n", i, vs);
                    continue;
                }
            }
            xkb_keysym_t sym = XKB_KEY_NoSymbol;
            char *ks = NULL;
            if (get_string(e, "key", &ks) == 0) {
                sym = parse_keysym(ks);
                free(ks);
            } else {
                int64_t ki;
                if (get_int(e, "key", &ki) == 0 && ki != 0) {
                    sym = (xkb_keysym_t)ki;
                }
            }
            if (sym == XKB_KEY_NoSymbol) {
                if (key_present(e, "key")) {
                    char vs[128] = {0};
                    diag_value_suffix(e, "key", vs, sizeof(vs));
                    fprintf(stderr, "canvas-wl: keybind #%d: bad key%s, skipping\n", i, vs);
                } else {
                    fprintf(stderr, "canvas-wl: keybind #%d: missing key, skipping\n", i);
                }
                continue;
            }
            char *as = NULL;
            KeyActionType at;
            if (get_string(e, "action", &as) != 0 ||
                    config_parse_action(as, &at) != 0) {
                if (as) {
                    fprintf(stderr, "canvas-wl: keybind #%d: bad action \"%s\", skipping\n", i, as);
                } else if (key_present(e, "action")) {
                    fprintf(stderr, "canvas-wl: keybind #%d: bad action (expected string), skipping\n", i);
                } else {
                    fprintf(stderr, "canvas-wl: keybind #%d: missing action, skipping\n", i);
                }
                free(as);
                continue;
            }
            free(as);
            char *cmd = NULL;
            if (at == action_run_command) {
                if (get_string(e, "cmd", &cmd) != 0) {
                    get_string(e, "command", &cmd);
                }
                if (!cmd) {
                    fprintf(stderr, "canvas-wl: keybind #%d: spawn without cmd\n", i);
                }
            }
            arr[count].modifier_mask = mods;
            arr[count].key_symbol = sym;
            arr[count].action_type = at;
            arr[count].command_to_run = cmd;
            count++;
        }
        /* Install (possibly empty) table, replacing defaults. */
        for (unsigned int i = 0; i < cfg->keybinding_count; i++) {
            free(cfg->keybindings[i].command_to_run);
        }
        free(cfg->keybindings);
        cfg->keybindings = arr;
        cfg->keybinding_count = count;
    }

    t = toml_table_in(root, "startup");
    if (t) {
        toml_array_t *cmds = toml_array_in(t, "commands");
        if (!cmds) {
            cmds = toml_array_in(t, "cmds");
        }
        if (cmds) {
            int n = toml_array_nelem(cmds);
            char **arr = NULL;
            unsigned int count = 0;
            if (n > 0) {
                arr = calloc((size_t)n, sizeof(char *));
                if (!arr) {
                    snprintf(errbuf, errbufsz, "out of memory");
                    toml_free(root);
                    return -1;
                }
            }
            for (int i = 0; i < n; i++) {
                toml_datum_t d = toml_string_at(cmds, i);
                if (!d.ok) {
                    fprintf(stderr, "canvas-wl: [startup] commands[%d]: not a string, skipping\n", i);
                    continue;
                }
                arr[count++] = d.u.s;
            }
            for (unsigned int i = 0; i < cfg->startup_command_count; i++) {
                free(cfg->startup_commands[i]);
            }
            free(cfg->startup_commands);
            cfg->startup_commands = arr;
            cfg->startup_command_count = count;
        } else if (key_present(t, "commands") || key_present(t, "cmds")) {
            fprintf(stderr, "canvas-wl: [startup] commands: expected array of strings, ignoring\n");
        }
    }

    toml_free(root);
    if (errbuf && errbufsz) {
        errbuf[0] = '\0';
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* config validation (`canvas validate`)                               */
/* ------------------------------------------------------------------ */

static void validate_unknown_keys(toml_table_t *t, const char *section,
        const char *const *known, size_t nknown, const char *path,
        config_diag *d) {
    int total = toml_table_nkval(t) + toml_table_narr(t) + toml_table_ntab(t);
    for (int i = 0; i < total; i++) {
        const char *k = toml_key_in(t, i);
        if (!k) {
            continue;
        }
        int ok = 0;
        for (size_t j = 0; j < nknown; j++) {
            if (!strcmp(k, known[j])) {
                ok = 1;
                break;
            }
        }
        if (!ok) {
            diag_add(d, "  %s: unknown key '%s'\n", section, k);
        }
    }
    (void)path;
}

/* Returns issue count (>=0), or -1 on fatal error (report holds reason). */
int config_validate(const char *path, char *report, size_t reportsz) {
    config_diag d = { report, reportsz, 0, 0 };
    if (report && reportsz) {
        report[0] = '\0';
    }
    if (!path || !path[0]) {
        if (report && reportsz) {
            snprintf(report, reportsz, "error: no config path given\n");
        }
        return -1;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (report && reportsz) {
            snprintf(report, reportsz, "%s: error: cannot open: %s\n", path,
                strerror(errno));
        }
        return -1;
    }
    char toml_err[512] = {0};
    toml_table_t *root = toml_parse_file(fp, toml_err, sizeof(toml_err));
    fclose(fp);
    if (!root) {
        if (report && reportsz) {
            snprintf(report, reportsz, "%s: error: %s\n", path,
                toml_err[0] ? toml_err : "toml parse error");
        }
        return -1;
    }

    diag_add(&d, "%s:\n", path);

    /* Unknown top-level tables/arrays (typo catcher). */
    {
        int total = toml_table_nkval(root) + toml_table_narr(root) +
            toml_table_ntab(root);
        for (int i = 0; i < total; i++) {
            const char *k = toml_key_in(root, i);
            if (!k) {
                continue;
            }
            if (strcmp(k, "window") && strcmp(k, "focus") &&
                    strcmp(k, "input") && strcmp(k, "cursor") &&
                    strcmp(k, "startup") && strcmp(k, "keybind")) {
                diag_add(&d, "  [root]: unknown table/array '%s'\n", k);
            }
        }
    }

    int64_t iv;
    int bv;
    unsigned long cv;
    unsigned int mv;
    char *sv = NULL;
    char vs[160];

    toml_table_t *t = toml_table_in(root, "window");
    if (t) {
        static const char *const known[] = { "border_width", "border_active",
            "border_inactive", "default_width", "default_height", "offset_x",
            "offset_y", "min_width", "min_height" };
        validate_unknown_keys(t, "[window]", known,
            sizeof(known) / sizeof(known[0]), path, &d);
        if (key_present(t, "border_width")) {
            if (get_int(t, "border_width", &iv) != 0 || iv < 0) {
                diag_value_suffix(t, "border_width", vs, sizeof(vs));
                diag_add(&d, "  [window] border_width: expected integer >= 0%s\n", vs);
            }
        }
        if (key_present(t, "border_active")) {
            if (get_color(t, "border_active", &cv) != 0) {
                diag_value_suffix(t, "border_active", vs, sizeof(vs));
                diag_add(&d, "  [window] border_active: bad color%s (want 0xRRGGBB or \"#rrggbb\")\n", vs);
            }
        }
        if (key_present(t, "border_inactive")) {
            if (get_color(t, "border_inactive", &cv) != 0) {
                diag_value_suffix(t, "border_inactive", vs, sizeof(vs));
                diag_add(&d, "  [window] border_inactive: bad color%s (want 0xRRGGBB or \"#rrggbb\")\n", vs);
            }
        }
        if (key_present(t, "default_width")) {
            if (get_int(t, "default_width", &iv) != 0 || iv <= 0) {
                diag_value_suffix(t, "default_width", vs, sizeof(vs));
                diag_add(&d, "  [window] default_width: expected integer > 0%s\n", vs);
            }
        }
        if (key_present(t, "default_height")) {
            if (get_int(t, "default_height", &iv) != 0 || iv <= 0) {
                diag_value_suffix(t, "default_height", vs, sizeof(vs));
                diag_add(&d, "  [window] default_height: expected integer > 0%s\n", vs);
            }
        }
        if (key_present(t, "offset_x")) {
            if (get_int(t, "offset_x", &iv) != 0) {
                diag_add(&d, "  [window] offset_x: expected integer\n");
            }
        }
        if (key_present(t, "offset_y")) {
            if (get_int(t, "offset_y", &iv) != 0) {
                diag_add(&d, "  [window] offset_y: expected integer\n");
            }
        }
        if (key_present(t, "min_width")) {
            if (get_int(t, "min_width", &iv) != 0 || iv <= 0) {
                diag_value_suffix(t, "min_width", vs, sizeof(vs));
                diag_add(&d, "  [window] min_width: expected integer > 0%s\n", vs);
            }
        }
        if (key_present(t, "min_height")) {
            if (get_int(t, "min_height", &iv) != 0 || iv <= 0) {
                diag_value_suffix(t, "min_height", vs, sizeof(vs));
                diag_add(&d, "  [window] min_height: expected integer > 0%s\n", vs);
            }
        }
    }

    t = toml_table_in(root, "focus");
    if (t) {
        static const char *const known[] = { "recent_order", "anim_duration_ms",
            "anim_fps", "interruption_mode" };
        validate_unknown_keys(t, "[focus]", known,
            sizeof(known) / sizeof(known[0]), path, &d);
        if (key_present(t, "recent_order")) {
            if (get_bool(t, "recent_order", &bv) != 0) {
                diag_add(&d, "  [focus] recent_order: expected bool (true/false)\n");
            }
        }
        if (key_present(t, "anim_duration_ms")) {
            if (get_int(t, "anim_duration_ms", &iv) != 0 || iv < 0) {
                diag_value_suffix(t, "anim_duration_ms", vs, sizeof(vs));
                diag_add(&d, "  [focus] anim_duration_ms: expected integer >= 0%s\n", vs);
            }
        }
        if (key_present(t, "anim_fps")) {
            if (get_int(t, "anim_fps", &iv) != 0 || iv <= 0) {
                diag_value_suffix(t, "anim_fps", vs, sizeof(vs));
                diag_add(&d, "  [focus] anim_fps: expected integer > 0%s\n", vs);
            }
        }
        if (key_present(t, "interruption_mode")) {
            if (get_int(t, "interruption_mode", &iv) != 0 || iv < 0 || iv > 2) {
                diag_value_suffix(t, "interruption_mode", vs, sizeof(vs));
                diag_add(&d, "  [focus] interruption_mode: expected integer 0..2%s\n", vs);
            }
        }
    }

    t = toml_table_in(root, "input");
    if (t) {
        static const char *const known[] = { "main_mod", "resize_mod",
            "btn_focus", "btn_move", "btn_camera", "btn_resize" };
        validate_unknown_keys(t, "[input]", known,
            sizeof(known) / sizeof(known[0]), path, &d);
        if (key_present(t, "main_mod")) {
            if (get_mods(t, "main_mod", &mv) != 0) {
                diag_value_suffix(t, "main_mod", vs, sizeof(vs));
                diag_add(&d, "  [input] main_mod: unknown modifier%s (want e.g. \"Mod4+Shift\", \"none\", or \"\")\n", vs);
            }
        }
        if (key_present(t, "resize_mod")) {
            if (get_mods(t, "resize_mod", &mv) != 0) {
                diag_value_suffix(t, "resize_mod", vs, sizeof(vs));
                diag_add(&d, "  [input] resize_mod: unknown modifier%s (want e.g. \"Mod1\", \"none\", or \"\")\n", vs);
            }
        }
        const char *btns[] = { "btn_focus", "btn_move", "btn_camera",
            "btn_resize" };
        for (size_t bi = 0; bi < sizeof(btns) / sizeof(btns[0]); bi++) {
            if (key_present(t, btns[bi])) {
                if (get_button(t, btns[bi], &mv) != 0) {
                    diag_value_suffix(t, btns[bi], vs, sizeof(vs));
                    diag_add(&d, "  [input] %s: unknown mouse button%s (want BTN_LEFT/RIGHT/MIDDLE/...)\n",
                        btns[bi], vs);
                }
            }
        }
    }

    t = toml_table_in(root, "cursor");
    if (t) {
        static const char *const known[] = { "theme", "size" };
        validate_unknown_keys(t, "[cursor]", known,
            sizeof(known) / sizeof(known[0]), path, &d);
        if (key_present(t, "theme")) {
            if (get_string(t, "theme", &sv) != 0) {
                diag_add(&d, "  [cursor] theme: expected string (empty = default)\n");
            } else {
                free(sv);
            }
        }
        if (key_present(t, "size")) {
            if (get_int(t, "size", &iv) != 0 || iv <= 0) {
                diag_value_suffix(t, "size", vs, sizeof(vs));
                diag_add(&d, "  [cursor] size: expected integer > 0%s\n", vs);
            }
        }
    }

    int valid_keybinds = 0;
    toml_array_t *kb = toml_array_in(root, "keybind");
    if (kb) {
        int n = toml_array_nelem(kb);
        unsigned int *seen_mods = n > 0 ?
            calloc((size_t)n, sizeof(*seen_mods)) : NULL;
        xkb_keysym_t *seen_syms = n > 0 ?
            calloc((size_t)n, sizeof(*seen_syms)) : NULL;
        int nseen = 0;
        static const char *const known[] = { "mod", "modifier", "key",
            "action", "cmd", "command" };
        for (int i = 0; i < n; i++) {
            toml_table_t *e = toml_table_at(kb, i);
            if (!e) {
                diag_add(&d, "  keybind #%d: not a table, skipped\n", i);
                continue;
            }
            {
                char sec[64];
                snprintf(sec, sizeof(sec), "keybind #%d", i);
                validate_unknown_keys(e, sec, known,
                    sizeof(known) / sizeof(known[0]), path, &d);
            }
            int ok_entry = 1;
            unsigned int mods = 0;
            int have_mods = (get_mods(e, "mod", &mods) == 0);
            if (!have_mods) {
                have_mods = (get_mods(e, "modifier", &mods) == 0);
            }
            if (!have_mods) {
                if (!key_present(e, "mod") && !key_present(e, "modifier")) {
                    mods = 0; /* no modifier: bare key */
                } else {
                    if (key_present(e, "mod")) {
                        diag_value_suffix(e, "mod", vs, sizeof(vs));
                    } else {
                        diag_value_suffix(e, "modifier", vs, sizeof(vs));
                    }
                    diag_add(&d, "  keybind #%d: bad mod%s (want e.g. \"Mod4+Shift\", \"none\", or \"\" for none)\n",
                        i, vs);
                    ok_entry = 0;
                }
            }
            xkb_keysym_t sym = XKB_KEY_NoSymbol;
            char *ks = NULL;
            if (get_string(e, "key", &ks) == 0) {
                sym = parse_keysym(ks);
                if (sym == XKB_KEY_NoSymbol) {
                    diag_add(&d, "  keybind #%d: unknown keysym \"%s\"\n", i, ks);
                    ok_entry = 0;
                }
                free(ks);
            } else {
                int64_t ki;
                if (get_int(e, "key", &ki) == 0 && ki != 0) {
                    sym = (xkb_keysym_t)ki;
                } else if (key_present(e, "key")) {
                    diag_value_suffix(e, "key", vs, sizeof(vs));
                    diag_add(&d, "  keybind #%d: bad key%s (want xkb name like \"Return\", \"q\")\n",
                        i, vs);
                    ok_entry = 0;
                } else {
                    diag_add(&d, "  keybind #%d: missing key (want xkb name like \"Return\")\n", i);
                    ok_entry = 0;
                }
            }
            KeyActionType at = action_run_command;
            int have_action = 0;
            char *as = NULL;
            if (get_string(e, "action", &as) == 0) {
                if (config_parse_action(as, &at) != 0) {
                    diag_add(&d, "  keybind #%d: unknown action \"%s\" (want spawn|close|quit|focus_next|focus_prev|reload)\n",
                        i, as);
                    ok_entry = 0;
                } else {
                    have_action = 1;
                }
                free(as);
            } else if (key_present(e, "action")) {
                diag_add(&d, "  keybind #%d: bad action (want string like \"spawn\")\n", i);
                ok_entry = 0;
            } else {
                diag_add(&d, "  keybind #%d: missing action (want spawn|close|quit|focus_next|focus_prev|reload)\n", i);
                ok_entry = 0;
            }
            if (have_action && at == action_run_command) {
                char *cmd = NULL;
                if (get_string(e, "cmd", &cmd) != 0) {
                    get_string(e, "command", &cmd);
                }
                if (!cmd) {
                    /* Runtime keeps the bind (key does nothing); warn but
                     * still count it for duplicates/summary. */
                    diag_add(&d, "  keybind #%d: spawn action needs cmd (e.g. cmd = \"alacritty\")\n", i);
                } else {
                    free(cmd);
                }
            } else if (have_action) {
                if (key_present(e, "cmd") || key_present(e, "command")) {
                    diag_add(&d, "  keybind #%d: note: cmd is ignored for non-spawn actions\n", i);
                }
            }
            if (ok_entry && seen_mods && seen_syms) {
                for (int s = 0; s < nseen; s++) {
                    if (seen_mods[s] == mods && seen_syms[s] == sym) {
                        char symbuf[64] = {0};
                        xkb_keysym_get_name(sym, symbuf, sizeof(symbuf));
                        diag_add(&d, "  keybind #%d: duplicate of an earlier bind (mod=0x%x key=%s)\n",
                            i, mods, symbuf[0] ? symbuf : "?");
                        break;
                    }
                }
                seen_mods[nseen] = mods;
                seen_syms[nseen] = sym;
                nseen++;
                valid_keybinds++;
            } else if (ok_entry) {
                valid_keybinds++;
            }
        }
        free(seen_mods);
        free(seen_syms);
    }

    t = toml_table_in(root, "startup");
    int startup_count = 0;
    if (t) {
        static const char *const known[] = { "commands", "cmds" };
        validate_unknown_keys(t, "[startup]", known,
            sizeof(known) / sizeof(known[0]), path, &d);
        toml_array_t *cmds = toml_array_in(t, "commands");
        if (!cmds) {
            cmds = toml_array_in(t, "cmds");
        }
        if (cmds) {
            int n = toml_array_nelem(cmds);
            for (int i = 0; i < n; i++) {
                toml_datum_t sd = toml_string_at(cmds, i);
                if (!sd.ok) {
                    diag_add(&d, "  [startup] commands[%d]: not a string, skipped\n", i);
                } else {
                    free(sd.u.s);
                    startup_count++;
                }
            }
        } else if (key_present(t, "commands") || key_present(t, "cmds")) {
            diag_add(&d, "  [startup] commands: expected array of strings\n");
        }
    }

    int issues = d.issues - 1; /* minus the "%s:\n" header line */
    if (issues < 0) {
        issues = 0;
    }
    if (issues == 0) {
        size_t used = d.len;
        snprintf(report + used, reportsz > used ? reportsz - used : 0,
            "  OK: valid (%d keybind%s, %d startup command%s)\n", valid_keybinds,
            valid_keybinds == 1 ? "" : "s", startup_count,
            startup_count == 1 ? "" : "s");
    } else {
        size_t used = d.len;
        snprintf(report + used, reportsz > used ? reportsz - used : 0,
            "  found %d problem%s\n", issues, issues == 1 ? "" : "s");
    }
    toml_free(root);
    return issues;
}

/* ------------------------------------------------------------------ */
/* paths + default file generation                                     */
/* ------------------------------------------------------------------ */

int config_default_path(char *buf, size_t bufsz) {
    if (!buf || bufsz == 0) {
        return -1;
    }
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        snprintf(buf, bufsz, "%s/canvaswl/canvaswl.toml", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0]) {
            return -1;
        }
        snprintf(buf, bufsz, "%s/.config/canvaswl/canvaswl.toml", home);
    }
    return 0;
}

static int mkdir_p(const char *dir) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t len = strlen(tmp);
    if (len == 0) {
        return -1;
    }
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* Compile-time install prefix (set via -DCANVASWL_PREFIX="..." in the
 * Makefile). Falls back to /usr/local when not defined (e.g. ad-hoc cc). */
#ifndef CANVASWL_PREFIX
#define CANVASWL_PREFIX "/usr/local"
#endif

/* Locate the shipped default template (config.toml.def). Search order:
 *   1. $CANVASWL_TEMPLATE (explicit override, handy for testing)
 *   2. <exe-dir>/config.toml.def (build tree / alongside binary)
 *   3. <exe-dir>/../share/canvaswl/config.toml.def (relocated install)
 *   4. CANVASWL_PREFIX/share/canvaswl/config.toml.def (compiled-in PREFIX)
 *   5. /usr/local/share/canvaswl/config.toml.def
 *   6. /usr/share/canvaswl/config.toml.def
 *   7. ./config.toml.def (cwd, dev runs)
 * Returns 0 and fills buf on success, -1 when nothing readable was found. */
static int find_default_template(char *buf, size_t bufsz) {
    const char *env = getenv("CANVASWL_TEMPLATE");
    if (env && env[0] && access(env, R_OK) == 0) {
        snprintf(buf, bufsz, "%s", env);
        return 0;
    }

    /* exe-dir based candidates (Linux /proc/self/exe). */
    char exe[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            char cand[PATH_MAX];
            snprintf(cand, sizeof(cand), "%s/config.toml.def", exe);
            if (access(cand, R_OK) == 0) {
                snprintf(buf, bufsz, "%s", cand);
                return 0;
            }
            snprintf(cand, sizeof(cand),
                "%s/../share/canvaswl/config.toml.def", exe);
            if (access(cand, R_OK) == 0) {
                snprintf(buf, bufsz, "%s", cand);
                return 0;
            }
        }
    }

    static const char *const fixed[] = {
        CANVASWL_PREFIX "/share/canvaswl/config.toml.def",
        "/usr/local/share/canvaswl/config.toml.def",
        "/usr/share/canvaswl/config.toml.def",
        "./config.toml.def",
    };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        if (access(fixed[i], R_OK) == 0) {
            snprintf(buf, bufsz, "%s", fixed[i]);
            return 0;
        }
    }
    return -1;
}

static int copy_file_contents(FILE *src, FILE *dst, const char *srcname,
        const char *dstname, char *errbuf, size_t errbufsz) {
    char tmp[8192];
    size_t r;
    while ((r = fread(tmp, 1, sizeof(tmp), src)) > 0) {
        if (fwrite(tmp, 1, r, dst) != r) {
            if (errbuf && errbufsz) {
                snprintf(errbuf, errbufsz, "cannot write %s: %s", dstname,
                    strerror(errno));
            }
            return -1;
        }
    }
    if (ferror(src)) {
        if (errbuf && errbufsz) {
            snprintf(errbuf, errbufsz, "cannot read %s: %s", srcname,
                strerror(errno));
        }
        return -1;
    }
    return 0;
}

int config_write_default_file(const char *path, char *errbuf,
        size_t errbufsz) {
    char template_path[PATH_MAX] = {0};
    if (find_default_template(template_path, sizeof(template_path)) != 0) {
        if (errbuf && errbufsz) {
            snprintf(errbuf, errbufsz,
                "no template found (looked for config.toml.def next to "
                "the binary, in %s/share/canvaswl/, "
                "/usr/local/share/canvaswl/, /usr/share/canvaswl/, ./, "
                "or $CANVASWL_TEMPLATE)",
                CANVASWL_PREFIX);
        }
        return -1;
    }

    /* mkdir -p the parent dir. */
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        if (mkdir_p(dir) != 0) {
            if (errbuf && errbufsz) {
                snprintf(errbuf, errbufsz, "cannot mkdir %s: %s", dir,
                    strerror(errno));
            }
            return -1;
        }
    }

    FILE *src = fopen(template_path, "r");
    if (!src) {
        if (errbuf && errbufsz) {
            snprintf(errbuf, errbufsz, "cannot read %s: %s", template_path,
                strerror(errno));
        }
        return -1;
    }
    FILE *fp = fopen(path, "w");
    if (!fp) {
        if (errbuf && errbufsz) {
            snprintf(errbuf, errbufsz, "cannot write %s: %s", path,
                strerror(errno));
        }
        fclose(src);
        return -1;
    }
    int rc = copy_file_contents(src, fp, template_path, path, errbuf,
        errbufsz);
    fclose(src);
    if (fclose(fp) != 0) {
        if (errbuf && errbufsz && rc == 0) {
            snprintf(errbuf, errbufsz, "cannot write %s: %s", path,
                strerror(errno));
        }
        return -1;
    }
    return rc;
}
