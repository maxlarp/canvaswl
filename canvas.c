/* canvas-wl - infinite-canvas Wayland compositor built on wlroots.
 *
 * Config is TOML-driven at runtime: ~/.config/canvaswl/canvaswl.toml
 * (auto-generated with defaults if missing). Reload live via the reload
 * keybind (default Mod4+Shift+R) or SIGHUP. See config.c.
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

// Runtime TOML config (config.h declares KeyBinding + canvas_config).
#include "config.h"

// Core structs

enum canvas_cursor_mode {
    CANVAS_CURSOR_PASSTHROUGH,
    CANVAS_CURSOR_MOVE,
    CANVAS_CURSOR_RESIZE,
    CANVAS_CURSOR_CAMERA,
};

struct window_size_entry {
    char *app_id; // owned, e.g. "alacritty", "firefox"
    int width, height; // last seen client geometry
};

struct canvas_server {
    struct wl_display *wl_display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_list toplevels;

    // layer-shell (wallpaper, bars, launchers like swaybg/waybar/fuzzel)
    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;
    struct wl_list layer_surfaces; // struct canvas_layer_surface.link
    // scene stacking: background/bottom below windows, top/overlay above
    struct wlr_scene_tree *layers[4]; // indexed by zwlr layer enum 0..3
    struct wlr_scene_tree *toplevel_parent;
    struct canvas_layer_surface *focused_layer; // exclusive kbd holder

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;

    struct wlr_seat *seat;
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener pointer_focus_change;
    struct wl_listener request_set_selection;
    struct wl_list keyboards;
    enum canvas_cursor_mode cursor_mode;
    struct canvas_toplevel *grabbed_toplevel;
    /* Button that started the current compositor gesture (move/resize/camera).
     * Its press/release are swallowed so the client never sees them (e.g.
     * Meta+right pans the camera without right-clicking the window).
     * 0 = none; deliberately NOT cleared by reset_cursor_mode so a release
     * is still swallowed when the modifier was let go mid-drag. */
    uint32_t gesture_button;
    double grab_x, grab_y; // grab offset (move/resize) or last pos (camera)
    struct wlr_box grab_geobox;
    uint32_t resize_edges;

    struct wlr_output_layout *output_layout;
    struct wlr_xdg_output_manager_v1 *xdg_output_manager;
    struct wl_list outputs;
    struct wl_listener new_output;

    // infinite-canvas camera: total pan offset applied to all windows
    int camera_x, camera_y;
    // output size used to center windows (first output, updated on hotplug)
    int output_width, output_height;

    struct canvas_toplevel *focused_toplevel;

    // frame-driven smooth pan animation (replaces X11 blocking usleep loop
    struct {
        bool active;
        struct canvas_toplevel *target;
        int direction; // +1 next / -1 prev, for coalesced bonus hop
        int64_t start_msec;
        int duration_ms;
        int delta_x, delta_y;
        int n;
        int cap;
        struct canvas_toplevel **clients;
        int *start_x;
        int *start_y;
        // coalesce mode: at most one bonus hop after current pan
        int pending_count;
        int pending_direction;
    } anim;

    int cascade; // placement offset so new windows don't stack exactly

    // Per-app size memory: last closed size keyed by xdg app_id, persisted to $XDG_CACHE_HOME/canvaswl/window_sizes
    struct window_size_entry *size_memory;
    int size_memory_count;
    int size_memory_cap;
    char *size_memory_path;

    canvas_config config;
    char *config_path;
};

struct canvas_output {
    struct wl_list link;
    struct canvas_server *server;
    struct wlr_output *wlr_output;
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
};

struct canvas_toplevel {
    struct wl_list link;
    struct canvas_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree; // outer frame, positioned at x,y
    struct wlr_scene_tree *win_tree;   // xdg surface, offset by border width
    struct wlr_scene_rect *border_top, *border_bottom;
    struct wlr_scene_rect *border_left, *border_right;
    int x, y;       // layout coords of scene_tree (outer frame incl. border)
    int width, height; // geometry (client content) size
    bool mapped;
    bool placed; // true once first-map view-centered placement ran
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
};

struct canvas_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

struct canvas_layer_surface {
    struct wl_list link;
    struct canvas_server *server;
    struct wlr_layer_surface_v1 *layer_surface;
    struct wlr_scene_layer_surface_v1 *scene_layer;
    struct wlr_output *output;
    bool mapped;
    bool exclusive; // holds keyboard focus until unmapped
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener new_popup;
};

struct canvas_keyboard {
    struct wl_list link;
    struct canvas_server *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

// MRU history + focus-cycle snapshot (ported from X11 version)

// Most-recently-used order: history[0] is the most recently focused.
static struct canvas_toplevel **window_usage_history = NULL;
static int window_usage_history_count = 0;
static int window_usage_history_cap = 0;

static int history_ensure_cap(int want) {
    if (want <= window_usage_history_cap) {
        return 0;
    }
    int ncap = window_usage_history_cap ? window_usage_history_cap * 2 : 16;
    while (ncap < want) {
        ncap *= 2;
    }
    struct canvas_toplevel **n = realloc(window_usage_history,
        (size_t)ncap * sizeof(*n));
    if (!n) {
        return -1;
    }
    window_usage_history = n;
    window_usage_history_cap = ncap;
    return 0;
}

static int managed_toplevel_count(struct canvas_server *server) {
    return wl_list_length(&server->toplevels);
}

static struct canvas_toplevel *managed_toplevel_at(struct canvas_server *server, int index) {
    struct canvas_toplevel *t;
    int i = 0;
    wl_list_for_each(t, &server->toplevels, link) {
        if (i == index) {
            return t;
        }
        i++;
    }
    return NULL;
}

static int find_managed_client_index(struct canvas_server *server,
        struct canvas_toplevel *wanted) {
    int i = 0;
    struct canvas_toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (t == wanted) {
            return i;
        }
        i++;
    }
    return -1;
}

static void remove_window_from_usage_history(struct canvas_toplevel *removed) {
    int pos = 0;
    while (pos < window_usage_history_count) {
        if (window_usage_history[pos] == removed) {
            for (int s = pos; s + 1 < window_usage_history_count; s++) {
                window_usage_history[s] = window_usage_history[s + 1];
            }
            window_usage_history_count--;
        } else {
            pos++;
        }
    }
}

static void record_window_in_usage_history(struct canvas_server *server,
        struct canvas_toplevel *used) {
    if (!used || find_managed_client_index(server, used) < 0) {
        return;
    }
    remove_window_from_usage_history(used);
    if (history_ensure_cap(window_usage_history_count + 1) != 0) {
        return;
    }
    for (int i = window_usage_history_count; i > 0; i--) {
        window_usage_history[i] = window_usage_history[i - 1];
    }
    window_usage_history[0] = used;
    window_usage_history_count++;
}

/* Cycle session snapshot so repeated presses walk 1 -> 2 -> 3 instead of
 * bouncing between the two most recent windows. */
static struct canvas_toplevel **focus_cycle_snapshot = NULL;
static int focus_cycle_snapshot_count = 0;
static int focus_cycle_snapshot_cap = 0;
static int focus_cycle_snapshot_cursor = -1;
static struct canvas_toplevel *focus_cycle_expected_focus = NULL;

static int snapshot_ensure_cap(int want) {
    if (want <= focus_cycle_snapshot_cap) {
        return 0;
    }
    int ncap = focus_cycle_snapshot_cap ? focus_cycle_snapshot_cap * 2 : 16;
    while (ncap < want) {
        ncap *= 2;
    }
    struct canvas_toplevel **n = realloc(focus_cycle_snapshot,
        (size_t)ncap * sizeof(*n));
    if (!n) {
        return -1;
    }
    focus_cycle_snapshot = n;
    focus_cycle_snapshot_cap = ncap;
    return 0;
}

static void snapshot_push(struct canvas_toplevel *t) {
    if (snapshot_ensure_cap(focus_cycle_snapshot_count + 1) != 0) {
        return;
    }
    focus_cycle_snapshot[focus_cycle_snapshot_count++] = t;
}

static int find_window_position_in_cycle_snapshot(struct canvas_toplevel *wanted) {
    for (int i = 0; i < focus_cycle_snapshot_count; i++) {
        if (focus_cycle_snapshot[i] == wanted) {
            return i;
        }
    }
    return -1;
}

static void rebuild_recent_cycle_snapshot_from_history(struct canvas_server *server) {
    focus_cycle_snapshot_count = 0;
    for (int h = 0; h < window_usage_history_count; h++) {
        struct canvas_toplevel *w = window_usage_history[h];
        if (find_managed_client_index(server, w) >= 0) {
            snapshot_push(w);
        }
    }
    struct canvas_toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (find_window_position_in_cycle_snapshot(t) < 0) {
            snapshot_push(t);
        }
    }
}

static void rebuild_nearest_cycle_snapshot_sorted_by_distance(
        struct canvas_server *server, int anchor_center_x, int anchor_center_y) {
    focus_cycle_snapshot_count = 0;
    int bw = server->config.window_border_width;
    int total = managed_toplevel_count(server);
    long *dist_sq = NULL;
    if (total > 0) {
        if (snapshot_ensure_cap(total) != 0) {
            return;
        }
        dist_sq = malloc((size_t)total * sizeof(*dist_sq));
        if (!dist_sq) {
            return;
        }
    }
    struct canvas_toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        int cx = t->x + (t->width + 2 * bw) / 2;
        int cy = t->y + (t->height + 2 * bw) / 2;
        long dx = (long)cx - (long)anchor_center_x;
        long dy = (long)cy - (long)anchor_center_y;
        long d = dx * dx + dy * dy;
        int ins = focus_cycle_snapshot_count;
        while (ins > 0 && dist_sq[ins - 1] > d) {
            focus_cycle_snapshot[ins] = focus_cycle_snapshot[ins - 1];
            dist_sq[ins] = dist_sq[ins - 1];
            ins--;
        }
        focus_cycle_snapshot[ins] = t;
        dist_sq[ins] = d;
        focus_cycle_snapshot_count++;
    }
    free(dist_sq);
}

static int is_cycle_snapshot_still_usable(struct canvas_server *server) {
    if (focus_cycle_snapshot_count != managed_toplevel_count(server) ||
            managed_toplevel_count(server) <= 0) {
        return 0;
    }
    if (focus_cycle_expected_focus != server->focused_toplevel) {
        return 0;
    }
    for (int i = 0; i < focus_cycle_snapshot_count; i++) {
        if (find_managed_client_index(server, focus_cycle_snapshot[i]) < 0) {
            return 0;
        }
    }
    return 1;
}

static struct canvas_toplevel *advance_cycle_cursor_one_step(struct canvas_server *server,
        int step_direction) {
    if (focus_cycle_snapshot_count <= 0) {
        return NULL;
    }
    int n = focus_cycle_snapshot_count;
    int next = (focus_cycle_snapshot_cursor + step_direction) % n;
    if (next < 0) {
        next += n;
    }
    focus_cycle_snapshot_cursor = next;
    struct canvas_toplevel *landed = focus_cycle_snapshot[next];
    if (find_managed_client_index(server, landed) < 0) {
        return NULL;
    }
    return landed;
}


// small helpers


static int64_t now_msec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Smoothstep easing: gentle start, fast middle, gentle stop. In/out 0..1.
static double ease_in_out_cubic_progress(double t) {
    if (t < 0.0) {
        t = 0.0;
    }
    if (t > 1.0) {
        t = 1.0;
    }
    if (t < 0.5) {
        return 4.0 * t * t * t;
    }
    double u = -2.0 * t + 2.0;
    return 1.0 - (u * u * u) / 2.0;
}

static void hex_to_rgba(unsigned long hex, float out[4]) {
    out[0] = ((hex >> 16) & 0xFF) / 255.0f;
    out[1] = ((hex >> 8) & 0xFF) / 255.0f;
    out[2] = (hex & 0xFF) / 255.0f;
    out[3] = 1.0f;
}

/* Keep only real shortcut bits so CapsLock-style state never breaks matching.
 * wlr modifier values are numerically identical to X11 masks for the four
 * bits we care about, so config lines carry over unchanged. */
static unsigned int clean_modifier_state(unsigned int raw) {
    const unsigned int relevant = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
        WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
    return raw & relevant;
}

/* Run an arbitrary shell command from config.h (e.g. "alacritty").
 * Uses /bin/sh -c so arguments and pipes work. Double-forks so the
 * compositor never accumulates zombies. */
static void run_configured_shell_command(const char *command_to_run) {
    if (!command_to_run || !command_to_run[0]) {
        fprintf(stderr, "empty command in keybinding, ignoring\n");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            execlp("/bin/sh", "sh", "-c", command_to_run, (char *)NULL);
            perror("sh -c failed");
            _exit(1);
        }
        _exit(0);
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

static void run_all_startup_commands(struct canvas_server *server) {
    for (unsigned int i = 0; i < server->config.startup_command_count; i++) {
        const char *cmd = server->config.startup_commands[i];
        if (!cmd || !cmd[0]) {
            continue;
        }
        run_configured_shell_command(cmd);
    }
}


// per-app size memory


/* Resolve the state file holding remembered window sizes:
 *   $XDG_CACHE_HOME/canvaswl/window_sizes else ~/.cache/canvaswl/window_sizes
 * Returns 0 on success. */
static int size_memory_default_path(char *buf, size_t bufsz) {
    if (!buf || bufsz == 0) {
        return -1;
    }
    const char *cache = getenv("XDG_CACHE_HOME");
    if (cache && cache[0]) {
        snprintf(buf, bufsz, "%s/canvaswl/window_sizes", cache);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0]) {
            return -1;
        }
        snprintf(buf, bufsz, "%s/.cache/canvaswl/window_sizes", home);
    }
    return 0;
}

static int size_memory_lookup(struct canvas_server *server, const char *app_id,
        int *w_out, int *h_out) {
    if (!server || !app_id || !app_id[0]) {
        return -1;
    }
    for (int i = 0; i < server->size_memory_count; i++) {
        if (server->size_memory[i].app_id &&
                strcmp(server->size_memory[i].app_id, app_id) == 0) {
            if (w_out) {
                *w_out = server->size_memory[i].width;
            }
            if (h_out) {
                *h_out = server->size_memory[i].height;
            }
            return 0;
        }
    }
    return -1;
}

/* Persist the whole table. Best effort: failures only warn, never break
 * window management. Called after every close (table is tiny). */
static void size_memory_save(struct canvas_server *server) {
    if (!server || !server->size_memory_path) {
        return;
    }
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", server->size_memory_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        char tmp[4096];
        snprintf(tmp, sizeof(tmp), "%s", dir);
        for (char *p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(tmp, 0755);
                *p = '/';
            }
        }
        mkdir(tmp, 0755);
    }
    FILE *fp = fopen(server->size_memory_path, "w");
    if (!fp) {
        fprintf(stderr, "canvas-wl: cannot save window sizes to %s\n",
            server->size_memory_path);
        return;
    }
    for (int i = 0; i < server->size_memory_count; i++) {
        struct window_size_entry *e = &server->size_memory[i];
        if (!e->app_id || !e->app_id[0] || e->width <= 0 || e->height <= 0) {
            continue;
        }
        if (strchr(e->app_id, '\t') || strchr(e->app_id, '\n')) {
            continue;
        }
        fprintf(fp, "%s\t%d\t%d\n", e->app_id, e->width, e->height);
    }
    fclose(fp);
}

static void size_memory_load(struct canvas_server *server) {
    if (!server || !server->size_memory_path) {
        return;
    }
    FILE *fp = fopen(server->size_memory_path, "r");
    if (!fp) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Format: "<app_id>\t<w>\t<h>". app_id may contain spaces, so
         * split from the right: last two whitespace-separated tokens are
         * the integers. */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }
        char *last_tab = strrchr(line, '\t');
        if (!last_tab) {
            last_tab = strrchr(line, ' ');
        }
        if (!last_tab) {
            continue;
        }
        char *h_str = last_tab + 1;
        *last_tab = '\0';
        char *prev_tab = strrchr(line, '\t');
        if (!prev_tab) {
            prev_tab = strrchr(line, ' ');
        }
        if (!prev_tab) {
            continue;
        }
        char *w_str = prev_tab + 1;
        *prev_tab = '\0';
        const char *app_id = line;
        while (*app_id == ' ' || *app_id == '\t') {
            app_id++;
        }
        if (!app_id[0]) {
            continue;
        }
        char *end = NULL;
        long w = strtol(w_str, &end, 10);
        if (*end != '\0' || w <= 0) {
            continue;
        }
        long h = strtol(h_str, &end, 10);
        if (*end != '\0' || h <= 0) {
            continue;
        }
        if (server->size_memory_count >= server->size_memory_cap) {
            int ncap = server->size_memory_cap ? server->size_memory_cap * 2 : 32;
            struct window_size_entry *n = realloc(server->size_memory,
                (size_t)ncap * sizeof(*n));
            if (!n) {
                break;
            }
            server->size_memory = n;
            server->size_memory_cap = ncap;
        }
        // De-dupe: file should already be unique, last wins.
        int dup = -1;
        for (int i = 0; i < server->size_memory_count; i++) {
            if (server->size_memory[i].app_id &&
                    strcmp(server->size_memory[i].app_id, app_id) == 0) {
                dup = i;
                break;
            }
        }
        if (dup >= 0) {
            server->size_memory[dup].width = (int)w;
            server->size_memory[dup].height = (int)h;
        } else {
            server->size_memory[server->size_memory_count].app_id = strdup(app_id);
            if (!server->size_memory[server->size_memory_count].app_id) {
                continue;
            }
            server->size_memory[server->size_memory_count].width = (int)w;
            server->size_memory[server->size_memory_count].height = (int)h;
            server->size_memory_count++;
        }
    }
    fclose(fp);
}

/* Remember the current size for app_id (MRU: most recent entry first,
 * capped so the file can't grow without bound). Persists to disk. */
static void size_memory_store(struct canvas_server *server, const char *app_id,
        int width, int height) {
    if (!server || !app_id || !app_id[0] || width <= 0 || height <= 0) {
        return;
    }
    if (strchr(app_id, '\t') || strchr(app_id, '\n')) {
        return;
    }
    // Move existing entry to front with the new size.
    for (int i = 0; i < server->size_memory_count; i++) {
        if (server->size_memory[i].app_id &&
                strcmp(server->size_memory[i].app_id, app_id) == 0) {
            server->size_memory[i].width = width;
            server->size_memory[i].height = height;
            struct window_size_entry hit = server->size_memory[i];
            memmove(&server->size_memory[1], &server->size_memory[0],
                (size_t)i * sizeof(hit));
            server->size_memory[0] = hit;
            size_memory_save(server);
            return;
        }
    }
    const int cap_hard = 512;
    if (server->size_memory_count >= cap_hard) {
        // Evict oldest (tail) to make room at front.
        free(server->size_memory[server->size_memory_count - 1].app_id);
        server->size_memory_count--;
    }
    if (server->size_memory_count >= server->size_memory_cap) {
        int ncap = server->size_memory_cap ? server->size_memory_cap * 2 : 32;
        if (ncap > cap_hard) {
            ncap = cap_hard;
        }
        struct window_size_entry *n = realloc(server->size_memory,
            (size_t)ncap * sizeof(*n));
        if (!n) {
            return;
        }
        server->size_memory = n;
        server->size_memory_cap = ncap;
    }
    memmove(&server->size_memory[1], &server->size_memory[0],
        (size_t)server->size_memory_count * sizeof(server->size_memory[0]));
    server->size_memory[0].app_id = strdup(app_id);
    if (!server->size_memory[0].app_id) {
        memmove(&server->size_memory[0], &server->size_memory[1],
            (size_t)server->size_memory_count * sizeof(server->size_memory[0]));
        return;
    }
    server->size_memory[0].width = width;
    server->size_memory[0].height = height;
    server->size_memory_count++;
    size_memory_save(server);
}

static void size_memory_free(struct canvas_server *server) {
    if (!server) {
        return;
    }
    for (int i = 0; i < server->size_memory_count; i++) {
        free(server->size_memory[i].app_id);
    }
    free(server->size_memory);
    server->size_memory = NULL;
    server->size_memory_count = 0;
    server->size_memory_cap = 0;
    free(server->size_memory_path);
    server->size_memory_path = NULL;
}

/* Snapshot the size of a toplevel into memory before it goes away.
 * Keyed by xdg app_id; windows without one (rare) can't be remembered. */
static void remember_toplevel_size(struct canvas_server *server,
        struct canvas_toplevel *toplevel) {
    if (!server || !toplevel || !toplevel->xdg_toplevel) {
        return;
    }
    const char *app_id = toplevel->xdg_toplevel->app_id;
    if (!app_id || !app_id[0]) {
        return;
    }
    if (toplevel->width <= 0 || toplevel->height <= 0) {
        return;
    }
    size_memory_store(server, app_id, toplevel->width, toplevel->height);
}


// focus + borders


static void toplevel_update_borders(struct canvas_toplevel *toplevel, bool active) {
    float color[4];
    canvas_config *cfg = &toplevel->server->config;
    hex_to_rgba(active ? cfg->window_border_color_active : cfg->window_border_color_inactive, color);
    int bw = cfg->window_border_width;
    int outer_w = toplevel->width + 2 * bw;
    int outer_h = toplevel->height + 2 * bw;
    if (outer_w < 1) {
        outer_w = 1;
    }
    if (outer_h < 1) {
        outer_h = 1;
    }

    wlr_scene_rect_set_size(toplevel->border_top, outer_w, bw);
    wlr_scene_rect_set_size(toplevel->border_bottom, outer_w, bw);
    wlr_scene_rect_set_size(toplevel->border_left, bw, toplevel->height);
    wlr_scene_rect_set_size(toplevel->border_right, bw, toplevel->height);

    wlr_scene_node_set_position(&toplevel->border_top->node, 0, 0);
    wlr_scene_node_set_position(&toplevel->border_bottom->node, 0, outer_h - bw);
    wlr_scene_node_set_position(&toplevel->border_left->node, 0, bw);
    wlr_scene_node_set_position(&toplevel->border_right->node, outer_w - bw, bw);

    wlr_scene_rect_set_color(toplevel->border_top, color);
    wlr_scene_rect_set_color(toplevel->border_bottom, color);
    wlr_scene_rect_set_color(toplevel->border_left, color);
    wlr_scene_rect_set_color(toplevel->border_right, color);

    wlr_scene_node_set_position(&toplevel->win_tree->node, bw, bw);
}

static void focus_toplevel(struct canvas_server *server,
        struct canvas_toplevel *toplevel) {
    if (!toplevel || find_managed_client_index(server, toplevel) < 0) {
        return;
    }
    if (toplevel == server->focused_toplevel) {
        record_window_in_usage_history(server, toplevel);
        return;
    }

    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    if (prev_surface) {
        struct wlr_xdg_toplevel *prev = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev) {
            wlr_xdg_toplevel_set_activated(prev, false);
        }
    }
    if (server->focused_toplevel) {
        toplevel_update_borders(server->focused_toplevel, false);
    }

    server->focused_toplevel = toplevel;
    record_window_in_usage_history(server, toplevel);

    // Raise above all other windows, keep list order in sync (front = top).
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    wl_list_remove(&toplevel->link);
    wl_list_insert(&server->toplevels, &toplevel->link);

    toplevel_update_borders(toplevel, true);
    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

    /* An exclusive layer surface (e.g. fuzzel) keeps the keyboard until it
     * goes away; the window is still tracked, raised and marked active, but
     * no keyboard enter is sent. A non-exclusive layer focus is dropped. */
    if (server->focused_layer &&
            !(server->focused_layer->mapped && server->focused_layer->exclusive)) {
        server->focused_layer = NULL;
    }
    bool layer_holds_kbd = server->focused_layer &&
        server->focused_layer->mapped && server->focused_layer->exclusive;
    if (layer_holds_kbd) {
        return;
    }

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
    if (keyboard) {
        wlr_seat_keyboard_notify_enter(seat, surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
    }
}

static void close_focused_toplevel(struct canvas_server *server) {
    struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
    if (!focused) {
        return;
    }
    struct wlr_xdg_toplevel *t = wlr_xdg_toplevel_try_from_wlr_surface(focused);
    if (t) {
        wlr_xdg_toplevel_send_close(t);
    }
}


// frame-driven camera animation


static void animation_free_snapshot(struct canvas_server *server) {
    free(server->anim.clients);
    free(server->anim.start_x);
    free(server->anim.start_y);
    server->anim.clients = NULL;
    server->anim.start_x = NULL;
    server->anim.start_y = NULL;
    server->anim.n = 0;
    server->anim.cap = 0;
}

static void animation_snapshot_clients(struct canvas_server *server) {
    animation_free_snapshot(server);
    int total = managed_toplevel_count(server);
    if (total <= 0) {
        return;
    }
    server->anim.clients = malloc((size_t)total * sizeof(*server->anim.clients));
    server->anim.start_x = malloc((size_t)total * sizeof(*server->anim.start_x));
    server->anim.start_y = malloc((size_t)total * sizeof(*server->anim.start_y));
    if (!server->anim.clients || !server->anim.start_x || !server->anim.start_y) {
        animation_free_snapshot(server);
        return;
    }
    server->anim.cap = total;
    server->anim.n = 0;
    struct canvas_toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        if (server->anim.n >= server->anim.cap) {
            break;
        }
        server->anim.clients[server->anim.n] = t;
        server->anim.start_x[server->anim.n] = t->x;
        server->anim.start_y[server->anim.n] = t->y;
        server->anim.n++;
    }
}

static void animation_kick_output_frames(struct canvas_server *server) {
    /* Frame-driven pan only progresses inside output_frame. The backend
     * emits no frames while idle, so an animation started from a bare
     * keypress would stall at t=0 until some unrelated damage (mouse
     * movement, client redraw) produced a frame. Explicitly request a
     * frame per output to keep the animation ticking on its own. */
    struct canvas_output *o;
    wl_list_for_each(o, &server->outputs, link) {
        wlr_output_schedule_frame(o->wlr_output);
    }
}

static void animation_start(struct canvas_server *server,
        struct canvas_toplevel *target, int direction) {
    int bw = server->config.window_border_width;
    int outer_w = target->width + 2 * bw;
    int outer_h = target->height + 2 * bw;
    int want_x = (server->output_width - outer_w) / 2;
    int want_y = (server->output_height - outer_h) / 2;
    int dx = want_x - target->x;
    int dy = want_y - target->y;

    // Already centered: just focus, no animation needed.
    if (dx == 0 && dy == 0) {
        server->anim.active = false;
        focus_toplevel(server, target);
        focus_cycle_expected_focus = target;
        return;
    }

    animation_snapshot_clients(server);
    server->anim.active = true;
    server->anim.target = target;
    server->anim.direction = direction;
    server->anim.start_msec = now_msec();
    server->anim.duration_ms = server->config.window_focus_animation_duration_ms;
    if (server->anim.duration_ms < 0) {
        server->anim.duration_ms = 0;
    }
    if (server->anim.duration_ms == 0) {
        server->anim.duration_ms = 1;
    }
    server->anim.delta_x = dx;
    server->anim.delta_y = dy;
    animation_kick_output_frames(server);
}

// Called from output_frame before commit. Advances the running pan.
static void animation_step(struct canvas_server *server);

// Shared tail: focus target + coalesced bonus hop (mode 1).
static void animation_finish(struct canvas_server *server) {
    struct canvas_toplevel *target = server->anim.target;
    int direction = server->anim.direction;
    server->anim.active = false;
    server->camera_x += server->anim.delta_x;
    server->camera_y += server->anim.delta_y;

    if (target && find_managed_client_index(server, target) >= 0) {
        focus_toplevel(server, target);
        focus_cycle_expected_focus = target;
    }

    if (server->config.window_focus_animation_interruption_mode != 1) {
        server->anim.pending_count = 0;
        animation_free_snapshot(server);
        return;
    }
    if (server->anim.pending_count <= 0) {
        animation_free_snapshot(server);
        return;
    }
    // Coalesce: a whole burst of spam becomes at most one bonus hop.
    server->anim.pending_count = 0;
    int bonus_dir = server->anim.pending_direction;
    (void)direction;
    struct canvas_toplevel *bonus = advance_cycle_cursor_one_step(server, bonus_dir);
    if (!bonus) {
        animation_free_snapshot(server);
        focus_cycle_snapshot_count = 0;
        focus_cycle_snapshot_cursor = -1;
        focus_cycle_expected_focus = NULL;
        return;
    }
    animation_start(server, bonus, bonus_dir);
    if (server->anim.active) {
        // Run synchronously to first frame below; focus happens at finish.
    } else {
        // Was already centered; animation_start focused it directly.
    }
}

static void animation_step(struct canvas_server *server) {
    if (!server->anim.active) {
        return;
    }
    // Target died mid-flight: abort session, focus stays where it is.
    if (!server->anim.target ||
            find_managed_client_index(server, server->anim.target) < 0) {
        server->anim.active = false;
        server->anim.pending_count = 0;
        animation_free_snapshot(server);
        focus_cycle_snapshot_count = 0;
        focus_cycle_snapshot_cursor = -1;
        focus_cycle_expected_focus = NULL;
        return;
    }
    int64_t now = now_msec();
    double t = (double)(now - server->anim.start_msec) /
        (double)server->anim.duration_ms;
    if (t >= 1.0) {
        for (int i = 0; i < server->anim.n; i++) {
            struct canvas_toplevel *c = server->anim.clients[i];
            if (find_managed_client_index(server, c) < 0) {
                continue;
            }
            c->x = server->anim.start_x[i] + server->anim.delta_x;
            c->y = server->anim.start_y[i] + server->anim.delta_y;
            wlr_scene_node_set_position(&c->scene_tree->node, c->x, c->y);
        }
        animation_finish(server);
        return;
    }
    double e = ease_in_out_cubic_progress(t);
    int cur_dx = (int)(server->anim.delta_x * e);
    int cur_dy = (int)(server->anim.delta_y * e);
    for (int i = 0; i < server->anim.n; i++) {
        struct canvas_toplevel *c = server->anim.clients[i];
        if (find_managed_client_index(server, c) < 0) {
            continue;
        }
        c->x = server->anim.start_x[i] + cur_dx;
        c->y = server->anim.start_y[i] + cur_dy;
        wlr_scene_node_set_position(&c->scene_tree->node, c->x, c->y);
    }
}

/* Shortcut entry point: pick next/previous window per configured order mode,
 * then smoothly pan the camera so it ends up centered. */
static void focus_camera_on_next_window_in_configured_order(
        struct canvas_server *server, int move_toward_older_window) {
    if (managed_toplevel_count(server) <= 0) {
        return;
    }

    int step_direction = move_toward_older_window ? 1 : -1;

    // A pan is already running: behavior depends on interruption mode.
    if (server->anim.active) {
        if (server->config.window_focus_animation_interruption_mode == 0) {
            return; // ignore presses that arrived mid-animation
        } else if (server->config.window_focus_animation_interruption_mode == 1) {
            server->anim.pending_count++;
            server->anim.pending_direction = step_direction;
            return; // coalesced into one bonus hop at finish
        }
        // mode 2: retarget the running animation from current positions.
        struct canvas_toplevel *next = advance_cycle_cursor_one_step(server, step_direction);
        if (!next) {
            focus_cycle_snapshot_count = 0;
            focus_cycle_snapshot_cursor = -1;
            focus_cycle_expected_focus = NULL;
            server->anim.active = false;
            server->anim.pending_count = 0;
            animation_free_snapshot(server);
            return;
        }
        animation_start(server, next, step_direction);
        return;
    }

    if (managed_toplevel_count(server) == 1) {
        struct canvas_toplevel *only = managed_toplevel_at(server, 0);
        focus_cycle_snapshot_count = 0;
        focus_cycle_snapshot_cursor = -1;
        focus_cycle_expected_focus = only;
        animation_start(server, only, step_direction);
        return;
    }

    if (!is_cycle_snapshot_still_usable(server)) {
        if (server->config.window_focus_cycle_uses_recent_order) {
            rebuild_recent_cycle_snapshot_from_history(server);
        } else {
            int anchor_x = server->output_width / 2;
            int anchor_y = server->output_height / 2;
            if (server->focused_toplevel &&
                    find_managed_client_index(server, server->focused_toplevel) >= 0) {
                struct canvas_toplevel *f = server->focused_toplevel;
                anchor_x = f->x + (f->width + 2 * server->config.window_border_width) / 2;
                anchor_y = f->y + (f->height + 2 * server->config.window_border_width) / 2;
            }
            rebuild_nearest_cycle_snapshot_sorted_by_distance(server, anchor_x, anchor_y);
        }
        if (focus_cycle_snapshot_count <= 0) {
            return;
        }
        int cur = find_window_position_in_cycle_snapshot(server->focused_toplevel);
        if (cur >= 0) {
            focus_cycle_snapshot_cursor = cur;
        } else if (step_direction > 0) {
            focus_cycle_snapshot_cursor = -1;
        } else {
            focus_cycle_snapshot_cursor = focus_cycle_snapshot_count;
        }
    }

    struct canvas_toplevel *target = advance_cycle_cursor_one_step(server, step_direction);
    if (!target) {
        focus_cycle_snapshot_count = 0;
        focus_cycle_snapshot_cursor = -1;
        focus_cycle_expected_focus = NULL;
        return;
    }
    animation_start(server, target, step_direction);
}

// Forget a toplevel everywhere (unmap + destroy share this).
static void forget_toplevel(struct canvas_server *server,
        struct canvas_toplevel *toplevel) {
    remove_window_from_usage_history(toplevel);
    if (server->grabbed_toplevel == toplevel) {
        server->grabbed_toplevel = NULL;
        server->cursor_mode = CANVAS_CURSOR_PASSTHROUGH;
    }
    if (server->anim.target == toplevel) {
        server->anim.target = NULL;
    }
    for (int i = 0; i < server->anim.n; i++) {
        if (server->anim.clients[i] == toplevel) {
            server->anim.clients[i] = NULL;
        }
    }
    if (server->focused_toplevel == toplevel) {
        server->focused_toplevel = NULL;
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
    if (focus_cycle_expected_focus == toplevel) {
        focus_cycle_expected_focus = NULL;
    }
}


// key dispatch


/* KeySyms are layout/shift dependent (e.g. pressing `q` yields XKB_KEY_q,
 * pressing Shift+`q` yields XKB_KEY_Q, pressing Shift+Tab yields
 * XKB_KEY_ISO_Left_Tab). Config lines use one spelling (usually the
 * uppercase X11 style), so normalize both sides before comparing. */
static xkb_keysym_t normalize_keysym_for_binding(xkb_keysym_t sym) {
    if (sym == XKB_KEY_ISO_Left_Tab) {
        return XKB_KEY_Tab;
    }
    return xkb_keysym_to_lower(sym);
}

/* Returns 0 = no binding matched, 1 = handled, 2 = logout requested,
 * 3 = config reload requested. */
static int handle_configured_key_press(struct canvas_server *server,
        xkb_keysym_t sym, unsigned int cleaned_mods) {
    xkb_keysym_t got = normalize_keysym_for_binding(sym);
    for (unsigned int i = 0; i < server->config.keybinding_count; i++) {
        const KeyBinding *b = &server->config.keybindings[i];
        if (got != normalize_keysym_for_binding(b->key_symbol) ||
                cleaned_mods != b->modifier_mask) {
            continue;
        }
        if (b->action_type == action_run_command) {
            run_configured_shell_command(b->command_to_run);
        } else if (b->action_type == action_close_focused_window) {
            close_focused_toplevel(server);
        } else if (b->action_type == action_quit_window_manager) {
            return 2;
        } else if (b->action_type == action_focus_next_window) {
            focus_camera_on_next_window_in_configured_order(server, 1);
        } else if (b->action_type == action_focus_previous_window) {
            focus_camera_on_next_window_in_configured_order(server, 0);
        } else if (b->action_type == action_reload_config) {
            return 3;
        }
        return 1; // first match wins
    }
    return 0;
}


// pointer: toplevel lookup + move/resize/camera


static struct canvas_toplevel *desktop_toplevel_at(struct canvas_server *server,
        double lx, double ly, struct wlr_surface **surface, double *sx, double *sy,
        struct canvas_layer_surface **layer_out) {
    if (layer_out) {
        *layer_out = NULL;
    }
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return NULL;
    }
    *surface = scene_surface->surface;
    struct wlr_scene_tree *tree = node->parent;
    while (tree && !tree->node.data) {
        tree = tree->node.parent;
    }
    if (!tree) {
        return NULL;
    }
    // Data belongs either to a managed xdg toplevel or to a layer surface.
    if (find_managed_client_index(server, tree->node.data) >= 0) {
        return tree->node.data;
    }
    if (layer_out) {
        struct canvas_layer_surface *l;
        wl_list_for_each(l, &server->layer_surfaces, link) {
            if (l->scene_layer && &l->scene_layer->tree->node == &tree->node) {
                *layer_out = l;
                break;
            }
        }
    }
    return NULL;
}

static void reset_cursor_mode(struct canvas_server *server) {
    server->cursor_mode = CANVAS_CURSOR_PASSTHROUGH;
    server->grabbed_toplevel = NULL;
}

static void process_cursor_move(struct canvas_server *server) {
    struct canvas_toplevel *t = server->grabbed_toplevel;
    if (!t || find_managed_client_index(server, t) < 0) {
        reset_cursor_mode(server);
        return;
    }
    t->x = (int)(server->cursor->x - server->grab_x);
    t->y = (int)(server->cursor->y - server->grab_y);
    wlr_scene_node_set_position(&t->scene_tree->node, t->x, t->y);
}

static void process_cursor_resize(struct canvas_server *server) {
    struct canvas_toplevel *t = server->grabbed_toplevel;
    if (!t || find_managed_client_index(server, t) < 0) {
        reset_cursor_mode(server);
        return;
    }
    /* NOTE: grab_geobox is the outer frame (incl. borders) at grab time,
     * grab_x/grab_y is the cursor offset from the grabbed border corner. */
    double border_x = server->cursor->x - server->grab_x;
    double border_y = server->cursor->y - server->grab_y;
    int new_left = server->grab_geobox.x;
    int new_right = server->grab_geobox.x + server->grab_geobox.width;
    int new_top = server->grab_geobox.y;
    int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

    if (server->resize_edges & WLR_EDGE_TOP) {
        new_top = (int)border_y;
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = (int)border_y;
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_left = (int)border_x;
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        new_right = (int)border_x;
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    int bw = server->config.window_border_width;
    int new_outer_w = new_right - new_left;
    int new_outer_h = new_bottom - new_top;
    int new_width = new_outer_w - 2 * bw;
    int new_height = new_outer_h - 2 * bw;
    if (new_width < server->config.minimum_window_width) {
        new_width = server->config.minimum_window_width;
    }
    if (new_height < server->config.minimum_window_height) {
        new_height = server->config.minimum_window_height;
    }
    // Keep top-left stable when clamped (matches X11 resize behavior).
    new_outer_w = new_width + 2 * bw;
    new_outer_h = new_height + 2 * bw;
    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_left = new_right - new_outer_w;
    }
    if (server->resize_edges & WLR_EDGE_TOP) {
        new_top = new_bottom - new_outer_h;
    }
    t->x = new_left;
    t->y = new_top;
    wlr_scene_node_set_position(&t->scene_tree->node, t->x, t->y);
    wlr_xdg_toplevel_set_size(t->xdg_toplevel, new_width, new_height);
}

static void process_cursor_camera(struct canvas_server *server) {
    double dx = server->cursor->x - server->grab_x;
    double dy = server->cursor->y - server->grab_y;
    if (dx == 0 && dy == 0) {
        return;
    }
    int idx = (int)dx, idy = (int)dy;
    struct canvas_toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        t->x += idx;
        t->y += idy;
        wlr_scene_node_set_position(&t->scene_tree->node, t->x, t->y);
    }
    if (server->anim.active) {
        for (int i = 0; i < server->anim.n; i++) {
            if (server->anim.clients[i]) {
                server->anim.start_x[i] += idx;
                server->anim.start_y[i] += idy;
            }
        }
    }
    server->camera_x += idx;
    server->camera_y += idy;
    server->grab_x = server->cursor->x;
    server->grab_y = server->cursor->y;
}

static void process_cursor_motion(struct canvas_server *server, uint32_t time) {
    if (server->cursor_mode == CANVAS_CURSOR_MOVE) {
        process_cursor_move(server);
        return;
    } else if (server->cursor_mode == CANVAS_CURSOR_RESIZE) {
        process_cursor_resize(server);
        return;
    } else if (server->cursor_mode == CANVAS_CURSOR_CAMERA) {
        process_cursor_camera(server);
        return;
    }

    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    desktop_toplevel_at(server, server->cursor->x, server->cursor->y,
        &surface, &sx, &sy, NULL);
    if (!surface) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(seat);
    }
}

static void begin_interactive_move(struct canvas_server *server,
        struct canvas_toplevel *toplevel) {
    server->grabbed_toplevel = toplevel;
    server->cursor_mode = CANVAS_CURSOR_MOVE;
    server->grab_x = server->cursor->x - toplevel->x;
    server->grab_y = server->cursor->y - toplevel->y;
}

static void begin_interactive_resize(struct canvas_server *server,
        struct canvas_toplevel *toplevel, uint32_t edges) {
    server->grabbed_toplevel = toplevel;
    server->cursor_mode = CANVAS_CURSOR_RESIZE;
    int bw = server->config.window_border_width;
    int outer_w = toplevel->width + 2 * bw;
    int outer_h = toplevel->height + 2 * bw;
    double border_x = toplevel->x + ((edges & WLR_EDGE_RIGHT) ? outer_w : 0);
    double border_y = toplevel->y + ((edges & WLR_EDGE_BOTTOM) ? outer_h : 0);
    server->grab_x = server->cursor->x - border_x;
    server->grab_y = server->cursor->y - border_y;
    server->grab_geobox.x = toplevel->x;
    server->grab_geobox.y = toplevel->y;
    server->grab_geobox.width = outer_w;
    server->grab_geobox.height = outer_h;
    server->resize_edges = edges;
}

static void begin_interactive_camera(struct canvas_server *server) {
    server->grabbed_toplevel = NULL;
    server->cursor_mode = CANVAS_CURSOR_CAMERA;
    server->grab_x = server->cursor->x;
    server->grab_y = server->cursor->y;
}


// layer-shell: wallpaper, bars, launchers (swaybg/waybar/fuzzel)


static void xdg_popup_commit(struct wl_listener *listener, void *data);
static void xdg_popup_destroy(struct wl_listener *listener, void *data);

static void layer_focus_surface(struct canvas_server *server,
        struct canvas_layer_surface *layer) {
    if (!layer || !layer->mapped) {
        return;
    }
    server->focused_layer = layer;
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    struct wlr_surface *surface = layer->layer_surface->surface;
    if (keyboard) {
        wlr_seat_keyboard_notify_enter(server->seat, surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
    }
}

// Exclusive layer went away: hand keyboard back to the focused window.
static void layer_restore_focus(struct canvas_server *server) {
    server->focused_layer = NULL;
    struct canvas_toplevel *t = server->focused_toplevel;
    if (!t || find_managed_client_index(server, t) < 0) {
        return;
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    struct wlr_surface *surface = t->xdg_toplevel->base->surface;
    if (keyboard) {
        wlr_seat_keyboard_notify_enter(server->seat, surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
    }
}

static void layer_output_box(struct canvas_server *server,
        struct wlr_output *output, struct wlr_box *box) {
    if (output && output->width > 0 && output->height > 0) {
        box->x = 0;
        box->y = 0;
        box->width = output->width;
        box->height = output->height;
    } else {
        // No output (e.g. headless test): arrange against the fallback size.
        box->x = 0;
        box->y = 0;
        box->width = server->output_width > 0 ? server->output_width : 1920;
        box->height = server->output_height > 0 ? server->output_height : 1080;
    }
}

/* Arrange every mapped layer surface for one output (NULL = fallback group).
 * Exclusive zones are carved out in stacking order so bars/panels reserve
 * space for the next layer, matching the standard layer-shell algorithm. */
static void arrange_layers(struct canvas_server *server, struct wlr_output *output) {
    struct wlr_box full, usable;
    layer_output_box(server, output, &full);
    usable = full;
    for (int layer_idx = 0; layer_idx < 4; layer_idx++) {
        struct canvas_layer_surface *l;
        wl_list_for_each(l, &server->layer_surfaces, link) {
            /* NOTE: unmapped surfaces are included on purpose - the initial
             * configure must go out before the client attaches its first
             * buffer, and map only happens after that. */
            if (l->output != output) {
                continue;
            }
            if ((int)l->layer_surface->current.layer != layer_idx) {
                continue;
            }
            if (l->scene_layer->tree->node.parent != server->layers[layer_idx]) {
                wlr_scene_node_reparent(&l->scene_layer->tree->node,
                    server->layers[layer_idx]);
            }
            wlr_scene_layer_surface_v1_configure(l->scene_layer, &full, &usable);
        }
    }
}

static void arrange_all_layers(struct canvas_server *server) {
    struct canvas_output *o;
    bool any_output = false;
    wl_list_for_each(o, &server->outputs, link) {
        any_output = true;
        arrange_layers(server, o->wlr_output);
    }
    // Surfaces without an output (or headless with none) use the fallback.
    arrange_layers(server, NULL);
    (void)any_output;
}

static void layer_map(struct wl_listener *listener, void *data) {
    struct canvas_layer_surface *layer = wl_container_of(listener, layer, map);
    (void)data;
    struct canvas_server *server = layer->server;
    layer->mapped = true;
    wlr_scene_node_set_enabled(&layer->scene_layer->tree->node, true);
    arrange_layers(server, layer->output);
    layer->exclusive = (layer->layer_surface->current.keyboard_interactive ==
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    if (layer->exclusive) {
        layer_focus_surface(server, layer);
    }
    printf("canvas-wl: mapped layer surface '%s' layer=%d\n",
        layer->layer_surface->namespace ? layer->layer_surface->namespace : "?",
        (int)layer->layer_surface->current.layer);
}

static void layer_unmap(struct wl_listener *listener, void *data) {
    struct canvas_layer_surface *layer = wl_container_of(listener, layer, unmap);
    (void)data;
    struct canvas_server *server = layer->server;
    layer->mapped = false;
    wlr_scene_node_set_enabled(&layer->scene_layer->tree->node, false);
    arrange_layers(server, layer->output);
    if (server->focused_layer == layer) {
        layer_restore_focus(server);
    }
}

static void layer_commit(struct wl_listener *listener, void *data) {
    struct canvas_layer_surface *layer = wl_container_of(listener, layer, commit);
    (void)data;
    struct canvas_server *server = layer->server;
    struct wlr_layer_surface_v1 *surface = layer->layer_surface;
    layer->exclusive = (surface->current.keyboard_interactive ==
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    uint32_t committed = surface->current.committed;
    if (!surface->configured ||
            (committed & (WLR_LAYER_SURFACE_V1_STATE_LAYER |
                WLR_LAYER_SURFACE_V1_STATE_ANCHOR |
                WLR_LAYER_SURFACE_V1_STATE_EXCLUSIVE_ZONE |
                WLR_LAYER_SURFACE_V1_STATE_MARGIN |
                WLR_LAYER_SURFACE_V1_STATE_DESIRED_SIZE |
                WLR_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY))) {
        arrange_layers(server, layer->output);
    }
}

static void layer_destroy(struct wl_listener *listener, void *data) {
    struct canvas_layer_surface *layer = wl_container_of(listener, layer, destroy);
    (void)data;
    struct canvas_server *server = layer->server;
    if (server->focused_layer == layer) {
        server->focused_layer = NULL;
    }
    wl_list_remove(&layer->link);
    wl_list_remove(&layer->map.link);
    wl_list_remove(&layer->unmap.link);
    wl_list_remove(&layer->commit.link);
    wl_list_remove(&layer->destroy.link);
    wl_list_remove(&layer->new_popup.link);
    arrange_all_layers(server);
    free(layer);
}

static void layer_new_popup(struct wl_listener *listener, void *data) {
    struct canvas_layer_surface *layer = wl_container_of(listener, layer, new_popup);
    struct wlr_xdg_popup *xdg_popup = data;
    struct canvas_popup *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        return;
    }
    popup->xdg_popup = xdg_popup;
    xdg_popup->base->data =
        wlr_scene_xdg_surface_create(layer->scene_layer->tree, xdg_popup->base);
    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = xdg_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void server_new_layer_surface(struct wl_listener *listener, void *data) {
    struct canvas_server *server = wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *surface = data;

    /* Adopt an output when the client didn't pick one (or none exists yet -
     * headless keeps NULL and arranges against the fallback size). */
    if (!surface->output) {
        struct canvas_output *o;
        wl_list_for_each(o, &server->outputs, link) {
            surface->output = o->wlr_output;
            break;
        }
    }

    struct canvas_layer_surface *layer = calloc(1, sizeof(*layer));
    if (!layer) {
        return;
    }
    layer->server = server;
    layer->layer_surface = surface;
    layer->output = surface->output;

    /* Initial parent: bottom layer; corrected to the real layer (and kept
     * correct across layer changes) by arrange_layers on commit. */
    layer->scene_layer = wlr_scene_layer_surface_v1_create(
        server->layers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM], surface);
    layer->scene_layer->tree->node.data = layer;
    wlr_scene_node_set_enabled(&layer->scene_layer->tree->node, false);

    wl_list_insert(&server->layer_surfaces, &layer->link);

    layer->map.notify = layer_map;
    wl_signal_add(&surface->surface->events.map, &layer->map);
    layer->unmap.notify = layer_unmap;
    wl_signal_add(&surface->surface->events.unmap, &layer->unmap);
    layer->commit.notify = layer_commit;
    wl_signal_add(&surface->surface->events.commit, &layer->commit);
    layer->destroy.notify = layer_destroy;
    wl_signal_add(&surface->events.destroy, &layer->destroy);
    layer->new_popup.notify = layer_new_popup;
    wl_signal_add(&surface->events.new_popup, &layer->new_popup);
}

// wlroots listeners: cursor

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct canvas_server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base,
        event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct canvas_server *server =
        wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

static uint32_t canvas_keyboard_modifiers(struct canvas_server *server) {
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
    if (!kb) {
        return 0;
    }
    return wlr_keyboard_get_modifiers(kb);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct canvas_server *server =
        wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        /* End of a swallowed compositor gesture: don't forward a release
         * the client never saw a press for. */
        if (server->gesture_button != 0 &&
                event->button == server->gesture_button) {
            server->gesture_button = 0;
            reset_cursor_mode(server);
            return;
        }
        wlr_seat_pointer_notify_button(server->seat,
            event->time_msec, event->button, event->state);
        /* A client-initiated move/resize (xdg request, gesture_button == 0)
         * ends on release; an unrelated button going up must not cancel a
         * running compositor gesture. */
        if (server->gesture_button == 0) {
            reset_cursor_mode(server);
        }
        return;
    }

    // Press: focus whatever is under the cursor (plain click focuses).
    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct canvas_layer_surface *pressed_layer = NULL;
    struct canvas_toplevel *toplevel = desktop_toplevel_at(server,
        server->cursor->x, server->cursor->y, &surface, &sx, &sy,
        &pressed_layer);
    if (toplevel) {
        focus_toplevel(server, toplevel);
    } else if (pressed_layer && pressed_layer->mapped &&
            pressed_layer->layer_surface->current.keyboard_interactive !=
                ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
        // Click-to-focus for on-demand/exclusive layer surfaces.
        layer_focus_surface(server, pressed_layer);
    }

    uint32_t mods = canvas_keyboard_modifiers(server);
    bool main_held = (mods & server->config.main_modifier);
    bool resize_held = (mods & server->config.resize_modifier);

    /* Compositor gestures swallow the press so the client never sees it:
     * Meta+right pans the camera instead of right-clicking the window,
     * Meta+left moves instead of left-clicking, Alt+right resizes. */
    if (event->button == server->config.mouse_button_move && main_held && toplevel) {
        server->gesture_button = event->button;
        begin_interactive_move(server, toplevel);
        return;
    } else if (event->button == server->config.mouse_button_resize && resize_held && toplevel) {
        // Pick edges by cursor quadrant so any corner grips naturally.
        int bw = server->config.window_border_width;
        int outer_w = toplevel->width + 2 * bw;
        int outer_h = toplevel->height + 2 * bw;
        uint32_t edges = 0;
        edges |= (server->cursor->x < toplevel->x + outer_w / 2) ?
            WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
        edges |= (server->cursor->y < toplevel->y + outer_h / 2) ?
            WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
        server->gesture_button = event->button;
        begin_interactive_resize(server, toplevel, edges);
        return;
    } else if (event->button == server->config.mouse_button_camera && main_held) {
        // Camera pans from anywhere: on a window or on empty desktop.
        server->gesture_button = event->button;
        begin_interactive_camera(server);
        return;
    }

    wlr_seat_pointer_notify_button(server->seat,
        event->time_msec, event->button, event->state);
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
    struct canvas_server *server =
        wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(server->seat,
        event->time_msec, event->orientation, event->delta,
        event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
    struct canvas_server *server =
        wl_container_of(listener, server, cursor_frame);
    (void)data;
    wlr_seat_pointer_notify_frame(server->seat);
}


// wlroots listeners: outputs


static void output_frame(struct wl_listener *listener, void *data) {
    struct canvas_output *output = wl_container_of(listener, output, frame);
    (void)data;
    animation_step(output->server);
    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
        output->server->scene, output->wlr_output);
    wlr_scene_output_commit(scene_output, NULL);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
    if (output->server->anim.active) {
        /* Keep frames coming until the pan finishes; otherwise the
         * animation stalls mid-flight once the one kicked frame is
         * consumed and again waits on unrelated damage. */
        wlr_output_schedule_frame(output->wlr_output);
    }
}

static void output_request_state(struct wl_listener *listener, void *data) {
    struct canvas_output *output = wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    struct canvas_output *output = wl_container_of(listener, output, destroy);
    (void)data;
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    struct canvas_server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
        // Remember size for camera centering (first output wins).
        if (server->output_width <= 0) {
            server->output_width = mode->width;
            server->output_height = mode->height;
        }
    }
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    // Fallback when the backend reports no mode (e.g. headless size 0).
    if (server->output_width <= 0) {
        server->output_width = 1920;
        server->output_height = 1080;
    }

    struct canvas_output *output = calloc(1, sizeof(*output));
    if (!output) {
        return;
    }
    output->wlr_output = wlr_output;
    output->server = server;
    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);
    wl_list_insert(&server->outputs, &output->link);

    struct wlr_output_layout_output *l_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    struct wlr_scene_output *scene_output =
        wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);

    /* Adopt layer surfaces that arrived before any output existed, then
     * (re)arrange everything onto the real output geometry. */
    struct canvas_layer_surface *l;
    wl_list_for_each(l, &server->layer_surfaces, link) {
        if (!l->output) {
            l->output = wlr_output;
            l->layer_surface->output = wlr_output;
        }
    }
    arrange_all_layers(server);
}


// wlroots listeners: xdg-shell


/* New windows appear with their center exactly on the center of the
 * currently visible view (the output viewport 0..W, 0..H), not at the
 * absolute canvas origin and not at the accumulated camera offset where
 * old windows may have been panned to. Runs once on first map so the real
 * client size (known after the initial commit) is used for centering.
 * Remaps keep their existing position. */
static void place_new_toplevel_in_view(struct canvas_server *server,
        struct canvas_toplevel *toplevel) {
    int out_w = server->output_width > 0 ? server->output_width : 1920;
    int out_h = server->output_height > 0 ? server->output_height : 1080;
    int w = toplevel->width > 0 ? toplevel->width : server->config.default_window_width;
    int h = toplevel->height > 0 ? toplevel->height : server->config.default_window_height;
    int bw = server->config.window_border_width;
    int outer_w = w + 2 * bw;
    int outer_h = h + 2 * bw;
    // Center of window lands on center of output (+ configured offset).
    toplevel->x = (out_w - outer_w) / 2 + server->config.default_window_x;
    toplevel->y = (out_h - outer_h) / 2 + server->config.default_window_y;
    wlr_scene_node_set_position(&toplevel->scene_tree->node,
        toplevel->x, toplevel->y);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    struct canvas_toplevel *toplevel = wl_container_of(listener, toplevel, map);
    (void)data;
    struct canvas_server *server = toplevel->server;
    toplevel->mapped = true;
    if (!toplevel->placed) {
        toplevel->placed = true;
        /* Late restore: if app_id arrived after the initial commit (so the
         * commit hook couldn't apply the remembered size), apply it now and
         * center on the remembered size. Clients that honor the configure
         * land exactly centered; ones that don't stay centered on whatever
         * size they actually committed. */
        {
            const char *app_id = toplevel->xdg_toplevel->app_id;
            int rw = 0, rh = 0;
            if (app_id && app_id[0] &&
                    size_memory_lookup(server, app_id, &rw, &rh) == 0 &&
                    rw > 0 && rh > 0 &&
                    (rw != toplevel->width || rh != toplevel->height)) {
                if (rw < server->config.minimum_window_width) {
                    rw = server->config.minimum_window_width;
                }
                if (rh < server->config.minimum_window_height) {
                    rh = server->config.minimum_window_height;
                }
                wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, rw, rh);
                toplevel->width = rw;
                toplevel->height = rh;
                toplevel_update_borders(toplevel, false);
            }
        }
        place_new_toplevel_in_view(server, toplevel);
    }
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
    wl_list_insert(&server->toplevels, &toplevel->link);
    focus_toplevel(server, toplevel);
    printf("canvas-wl: mapped new window %p\n", (void *)toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    struct canvas_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    (void)data;
    struct canvas_server *server = toplevel->server;
    remember_toplevel_size(server, toplevel);
    toplevel->mapped = false;
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    /* Hide/minimize: drop from visible list but keep the struct so a remap
     * restores it. forget_toplevel clears focus/grab/MRU references. */
    wl_list_remove(&toplevel->link);
    wl_list_init(&toplevel->link);
    forget_toplevel(server, toplevel);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    struct canvas_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
    (void)data;
    if (toplevel->xdg_toplevel->base->initial_commit) {
        /* Restore the last closed size for this app_id so the user doesn't
         * have to resize every launch. Clients without a remembered size
         * (or without an app_id) pick their own size as before. */
        int rw = 0, rh = 0;
        const char *app_id = toplevel->xdg_toplevel->app_id;
        if (app_id && app_id[0] &&
                size_memory_lookup(toplevel->server, app_id, &rw, &rh) == 0 &&
                rw > 0 && rh > 0) {
            if (rw < toplevel->server->config.minimum_window_width) {
                rw = toplevel->server->config.minimum_window_width;
            }
            if (rh < toplevel->server->config.minimum_window_height) {
                rh = toplevel->server->config.minimum_window_height;
            }
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, rw, rh);
        } else {
            // Let the client pick its own size first.
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
        }
    }
    struct wlr_box *geo = &toplevel->xdg_toplevel->base->geometry;
    if (geo->width > 0) {
        toplevel->width = geo->width;
    }
    if (geo->height > 0) {
        toplevel->height = geo->height;
    }
    bool active = (toplevel->server->focused_toplevel == toplevel);
    toplevel_update_borders(toplevel, active);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    struct canvas_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
    (void)data;
    struct canvas_server *server = toplevel->server;
    remember_toplevel_size(server, toplevel);
    if (toplevel->mapped) {
        wl_list_remove(&toplevel->link);
    }
    forget_toplevel(server, toplevel);
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    free(toplevel);
}

static void begin_interactive_for_client(struct canvas_toplevel *toplevel,
        enum canvas_cursor_mode mode, uint32_t edges) {
    struct canvas_server *server = toplevel->server;
    if (mode == CANVAS_CURSOR_MOVE) {
        begin_interactive_move(server, toplevel);
    } else {
        begin_interactive_resize(server, toplevel, edges);
    }
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    struct canvas_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    struct wlr_xdg_toplevel_move_event *event = data;
    struct canvas_server *server = toplevel->server;
    /* Drop stale requests: the client sends this async after a titlebar
     * press, which may arrive after the button was already released. Honoring
     * it would glue the window to the cursor with no button held (sticky
     * move that only the next click clears). */
    if (!wlr_seat_validate_pointer_grab_serial(server->seat,
            toplevel->xdg_toplevel->base->surface, event->serial)) {
        return;
    }
    begin_interactive_for_client(toplevel, CANVAS_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct canvas_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    struct canvas_server *server = toplevel->server;
    if (!wlr_seat_validate_pointer_grab_serial(server->seat,
            toplevel->xdg_toplevel->base->surface, event->serial)) {
        return;
    }
    begin_interactive_for_client(toplevel, CANVAS_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    struct canvas_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_maximize);
    (void)data;
    // No maximization in canvas (infinite canvas, no tiles) - ack configure.
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    struct canvas_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_fullscreen);
    (void)data;
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    struct canvas_server *server = wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct canvas_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    if (!toplevel) {
        return;
    }
    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->width = server->config.default_window_width;
    toplevel->height = server->config.default_window_height;
    toplevel->placed = false;

    /* Placeholder so the hidden node has a sane position; the real
     * view-centered placement happens on first map, when
     * the client size is known. */
    {
        int out_w = server->output_width > 0 ? server->output_width : 1920;
        int out_h = server->output_height > 0 ? server->output_height : 1080;
        int bw0 = server->config.window_border_width;
        toplevel->x = (out_w - (toplevel->width + 2 * bw0)) / 2 + server->config.default_window_x;
        toplevel->y = (out_h - (toplevel->height + 2 * bw0)) / 2 + server->config.default_window_y;
    }

    toplevel->scene_tree = wlr_scene_tree_create(server->toplevel_parent);
    toplevel->scene_tree->node.data = toplevel;

    float inactive[4];
    hex_to_rgba(server->config.window_border_color_inactive, inactive);
    int bw = server->config.window_border_width;
    toplevel->border_top = wlr_scene_rect_create(toplevel->scene_tree,
        toplevel->width + 2 * bw, bw, inactive);
    toplevel->border_bottom = wlr_scene_rect_create(toplevel->scene_tree,
        toplevel->width + 2 * bw, bw, inactive);
    toplevel->border_left = wlr_scene_rect_create(toplevel->scene_tree,
        bw, toplevel->height, inactive);
    toplevel->border_right = wlr_scene_rect_create(toplevel->scene_tree,
        bw, toplevel->height, inactive);

    toplevel->win_tree =
        wlr_scene_xdg_surface_create(toplevel->scene_tree, xdg_toplevel->base);
    xdg_toplevel->base->data = toplevel->win_tree;

    wlr_scene_node_set_position(&toplevel->scene_tree->node, toplevel->x, toplevel->y);
    toplevel_update_borders(toplevel, false);
    // Hidden until map (a client may create-then-map later).
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    wl_list_init(&toplevel->link);

    toplevel->map.notify = xdg_toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
    toplevel->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
    toplevel->request_move.notify = xdg_toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    struct canvas_popup *popup = wl_container_of(listener, popup, commit);
    (void)data;
    if (popup->xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    struct canvas_popup *popup = wl_container_of(listener, popup, destroy);
    (void)data;
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
    struct wlr_xdg_popup *xdg_popup = data;
    (void)listener;
    struct canvas_popup *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        return;
    }
    popup->xdg_popup = xdg_popup;
    struct wlr_xdg_surface *parent =
        wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    assert(parent != NULL);
    struct wlr_scene_tree *parent_tree = parent->data;
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = xdg_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

// Forward: live TOML reload (defined near main, used by key handler).
static void server_reload_config(struct canvas_server *server);


// wlroots listeners: keyboard + input + seat


static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    struct canvas_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    (void)data;
    struct canvas_server *server = keyboard->server;
    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(server->seat,
        &keyboard->wlr_keyboard->modifiers);
    /* Unstick interactive grabs the moment their modifier is gone: releasing
     * Super mid-drag drops the window instead of leaving it glued to the
     * cursor until some other click. Button release already resets (see
     * server_cursor_button), so a grab now ends on either. */
    if (server->cursor_mode != CANVAS_CURSOR_PASSTHROUGH) {
        uint32_t mods = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
        if (server->cursor_mode == CANVAS_CURSOR_RESIZE) {
            if (!(mods & server->config.resize_modifier)) {
                reset_cursor_mode(server);
            }
        } else if (!(mods & server->config.main_modifier)) {
            reset_cursor_mode(server);
        }
    }
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    struct canvas_keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct canvas_server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;
    struct wlr_seat *seat = server->seat;

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(
        keyboard->wlr_keyboard->xkb_state, keycode, &syms);

    bool handled = false;
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        uint32_t mods = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
        unsigned int cleaned = clean_modifier_state(mods);
        for (int i = 0; i < nsyms && !handled; i++) {
            int result = handle_configured_key_press(server, syms[i], cleaned);
            if (result == 2) {
                wl_display_terminate(server->wl_display);
                return;
            }
            if (result == 3) {
                server_reload_config(server);
                handled = true;
            }
            if (result == 1) {
                handled = true;
            }
        }
    }

    if (!handled) {
        wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec,
            event->keycode, event->state);
    }
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
    struct canvas_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    (void)data;
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void server_new_keyboard(struct canvas_server *server,
        struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

    struct canvas_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    if (!keyboard) {
        return;
    }
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    keyboard->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
    wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct canvas_server *server,
        struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
    struct canvas_server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_new_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        server_new_pointer(server, device);
        break;
    default:
        break;
    }
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
    struct canvas_server *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client =
        server->seat->pointer_state.focused_client;
    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface,
            event->hotspot_x, event->hotspot_y);
    }
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
    struct canvas_server *server =
        wl_container_of(listener, server, pointer_focus_change);
    struct wlr_seat_pointer_focus_change_event *event = data;
    (void)event;
    if (event->new_surface == NULL) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
    }
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
    struct canvas_server *server =
        wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}


// runtime config reload


/* Re-read the TOML file and swap it in. Startup commands are NOT re-run.
 * On parse failure the old config is kept and an error is logged. */
static void server_reload_config(struct canvas_server *server) {
    if (!server->config_path) {
        wlr_log(WLR_ERROR, "reload: no config path stored");
        return;
    }
    canvas_config fresh = {0};
    char err[512] = {0};
    if (config_load(server->config_path, &fresh, err, sizeof(err)) != 0) {
        wlr_log(WLR_ERROR, "reload: %s (keeping old config)",
            err[0] ? err : "load failed");
        config_free_contents(&fresh);
        return;
    }

    bool cursor_changed = false;
    const char *old_theme = server->config.cursor_theme_name;
    const char *new_theme = fresh.cursor_theme_name;
    if (server->config.cursor_size != fresh.cursor_size ||
            ((old_theme == NULL) != (new_theme == NULL)) ||
            (old_theme && new_theme && strcmp(old_theme, new_theme) != 0)) {
        cursor_changed = true;
    }

    config_free_contents(&server->config);
    server->config = fresh;

    if (cursor_changed && server->cursor_mgr) {
        struct wlr_xcursor_manager *nm = wlr_xcursor_manager_create(
            server->config.cursor_theme_name, server->config.cursor_size);
        if (nm) {
            wlr_xcursor_manager_destroy(server->cursor_mgr);
            server->cursor_mgr = nm;
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
                "default");
        } else {
            wlr_log(WLR_ERROR, "reload: bad cursor theme/size, keeping old");
        }
    }

    // Re-apply border width/colors to every managed window.
    struct canvas_toplevel *t;
    wl_list_for_each(t, &server->toplevels, link) {
        toplevel_update_borders(t, t == server->focused_toplevel);
    }
    wlr_log(WLR_INFO, "reload: applied %s", server->config_path);
    printf("canvas-wl: reloaded %s\n", server->config_path);
}

static int handle_sighup_signal(int signal_number, void *data) {
    (void)signal_number;
    struct canvas_server *server = data;
    server_reload_config(server);
    return 0;
}


// main


static void print_usage(const char *prog) {
    printf("Usage:\n"
        "  %s [-s startup command] [-c config path]\n"
        "  %s validate [-c config path]   validate config and print errors\n"
        "  %s --validate [-c config path]\n"
        "  %s -h | --help\n",
        prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    wlr_log_init(WLR_DEBUG, NULL);
    char *startup_cmd = NULL;
    char *config_path_arg = NULL;
    int validate_mode = 0;

    /* Subcommand form: `canvas validate [...]`. Shift it out so getopt
     * below only sees flags. Also accepts `check` as an alias. */
    if (argc >= 2 && (!strcmp(argv[1], "validate") || !strcmp(argv[1], "check"))) {
        validate_mode = 1;
        for (int i = 1; i < argc; i++) {
            argv[i] = argv[i + 1];
        }
        argc--;
    }

    static struct option long_opts[] = {
        { "config", required_argument, NULL, 'c' },
        { "validate", no_argument, NULL, 'v' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "s:c:hv", long_opts, NULL)) != -1) {
        switch (c) {
        case 's':
            startup_cmd = optarg;
            break;
        case 'c':
            config_path_arg = optarg;
            break;
        case 'v':
            validate_mode = 1;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return 0;
        }
    }
    // Trailing form: `canvas -c path validate`.
    if (!validate_mode && optind < argc &&
            (!strcmp(argv[optind], "validate") || !strcmp(argv[optind], "check"))) {
        validate_mode = 1;
        optind++;
    }
    if (optind < argc) {
        print_usage(argv[0]);
        return 1;
    }

    if (validate_mode) {
        char default_path[4096] = {0};
        const char *cfg_path = config_path_arg;
        if (!cfg_path) {
            if (config_default_path(default_path, sizeof(default_path)) != 0) {
                fprintf(stderr, "canvas-wl: cannot resolve config path\n");
                return 1;
            }
            cfg_path = default_path;
        }
        char report[16384] = {0};
        int rc = config_validate(cfg_path, report, sizeof(report));
        if (rc < 0) {
            fprintf(stderr, "%s", report[0] ? report : "validation failed\n");
            return 1;
        }
        printf("%s", report[0] ? report : "OK\n");
        return rc == 0 ? 0 : 1;
    }

    struct canvas_server server = {0};

    /* Resolve config path: explicit flag wins, else the XDG default
     * (~/.config/canvaswl/canvaswl.toml). A missing default file is
     * auto-generated so first run just works. */
    char default_path[4096] = {0};
    const char *cfg_path = config_path_arg;
    if (!cfg_path) {
        if (config_default_path(default_path, sizeof(default_path)) != 0) {
            fprintf(stderr, "canvas-wl: cannot resolve config path, using defaults\n");
        } else {
            cfg_path = default_path;
            if (access(cfg_path, F_OK) != 0) {
                char err[512] = {0};
                if (config_write_default_file(cfg_path, err, sizeof(err)) != 0) {
                    fprintf(stderr, "canvas-wl: cannot generate %s: %s\n",
                        cfg_path, err[0] ? err : "unknown error");
                } else {
                    printf("canvas-wl: generated default config at %s\n", cfg_path);
                }
            }
        }
    }
    if (cfg_path) {
        server.config_path = strdup(cfg_path);
        char err[512] = {0};
        if (config_load(cfg_path, &server.config, err, sizeof(err)) != 0) {
            if (config_path_arg) {
                fprintf(stderr, "canvas-wl: %s\n",
                    err[0] ? err : "config load failed");
                free(server.config_path);
                config_free_contents(&server.config);
                return 1;
            }
            fprintf(stderr, "canvas-wl: %s; using defaults\n",
                err[0] ? err : "config load failed");
        } else {
            printf("canvas-wl: loaded %s\n", cfg_path);
        }
    } else {
        config_init_defaults(&server.config);
    }
    /* Remembered window sizes (per app_id). Loaded once at startup so
     * reopened windows come back at their last closed size. */
    {
        char size_path[4096] = {0};
        if (size_memory_default_path(size_path, sizeof(size_path)) == 0) {
            server.size_memory_path = strdup(size_path);
            size_memory_load(&server);
        }
    }
    server.output_width = 1920;
    server.output_height = 1080;
    server.wl_display = wl_display_create();
    server.backend = wlr_backend_autocreate(
        wl_display_get_event_loop(server.wl_display), NULL);
    if (!server.backend) {
        wlr_log(WLR_ERROR, "failed to create wlr_backend");
        config_free_contents(&server.config);
        free(server.config_path);
        size_memory_free(&server);
        return 1;
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer) {
        wlr_log(WLR_ERROR, "failed to create wlr_renderer");
        config_free_contents(&server.config);
        free(server.config_path);
        size_memory_free(&server);
        return 1;
    }
    wlr_renderer_init_wl_display(server.renderer, server.wl_display);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (!server.allocator) {
        wlr_log(WLR_ERROR, "failed to create wlr_allocator");
        config_free_contents(&server.config);
        free(server.config_path);
        size_memory_free(&server);
        return 1;
    }

    wlr_compositor_create(server.wl_display, 5, server.renderer);
    wlr_subcompositor_create(server.wl_display);
    wlr_data_device_manager_create(server.wl_display);

    /* Screen capture: wl-screenrec/wf-recorder need zwlr-screencopy-manager
     * (wlr-screencopy-unstable-v1); modern tools (OBS portal,
     * gpu-screen-recorder, newer wl-screenrec) use ext-image-copy-capture-v1.
     * wlr_renderer_init_wl_display already provides linux-dmabuf. */
    wlr_screencopy_manager_v1_create(server.wl_display);
    wlr_export_dmabuf_manager_v1_create(server.wl_display);
    wlr_ext_output_image_capture_source_manager_v1_create(server.wl_display, 1);
    wlr_ext_image_copy_capture_manager_v1_create(server.wl_display, 1);

    server.output_layout = wlr_output_layout_create(server.wl_display);
    server.xdg_output_manager = wlr_xdg_output_manager_v1_create(
        server.wl_display, server.output_layout);
    wl_list_init(&server.outputs);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.scene = wlr_scene_create();
    server.scene_layout =
        wlr_scene_attach_output_layout(server.scene, server.output_layout);

    /* Scene stacking, bottom to top: background, bottom, windows, top,
     * overlay. Toplevel raise-to-top stays inside toplevel_parent, so
     * windows can never cover panels/launchers or sink under wallpaper. */
    server.layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] =
        wlr_scene_tree_create(&server.scene->tree);
    server.layers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] =
        wlr_scene_tree_create(&server.scene->tree);
    server.toplevel_parent = wlr_scene_tree_create(&server.scene->tree);
    server.layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP] =
        wlr_scene_tree_create(&server.scene->tree);
    server.layers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] =
        wlr_scene_tree_create(&server.scene->tree);

    wl_list_init(&server.toplevels);
    server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
    server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
    server.new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

    wl_list_init(&server.layer_surfaces);
    server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
    server.new_layer_surface.notify = server_new_layer_surface;
    wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    server.cursor_mgr = wlr_xcursor_manager_create(
        server.config.cursor_theme_name, server.config.cursor_size);
    wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");

    server.cursor_mode = CANVAS_CURSOR_PASSTHROUGH;
    server.cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute,
        &server.cursor_motion_absolute);
    server.cursor_button.notify = server_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    wl_list_init(&server.keyboards);
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);
    server.seat = wlr_seat_create(server.wl_display, "seat0");
    server.request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
    server.pointer_focus_change.notify = seat_pointer_focus_change;
    wl_signal_add(&server.seat->pointer_state.events.focus_change,
        &server.pointer_focus_change);
    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection,
        &server.request_set_selection);

    const char *socket = wl_display_add_socket_auto(server.wl_display);
    if (!socket) {
        wlr_backend_destroy(server.backend);
        config_free_contents(&server.config);
        free(server.config_path);
        size_memory_free(&server);
        return 1;
    }

    if (!wlr_backend_start(server.backend)) {
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.wl_display);
        config_free_contents(&server.config);
        free(server.config_path);
        size_memory_free(&server);
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    // XWayland intentionally not started in v1 (Wayland-native only).
    run_all_startup_commands(&server);
    if (startup_cmd) {
        run_configured_shell_command(startup_cmd);
    }

    wl_event_loop_add_signal(wl_display_get_event_loop(server.wl_display),
        SIGHUP, handle_sighup_signal, &server);

    wlr_log(WLR_INFO, "canvas-wl running on WAYLAND_DISPLAY=%s", socket);
    puts("canvas-wl running...");
    wl_display_run(server.wl_display);

    wl_display_destroy_clients(server.wl_display);
    wl_list_remove(&server.new_xdg_toplevel.link);
    wl_list_remove(&server.new_xdg_popup.link);
    wl_list_remove(&server.new_layer_surface.link);
    wl_list_remove(&server.cursor_motion.link);
    wl_list_remove(&server.cursor_motion_absolute.link);
    wl_list_remove(&server.cursor_button.link);
    wl_list_remove(&server.cursor_axis.link);
    wl_list_remove(&server.cursor_frame.link);
    wl_list_remove(&server.new_input.link);
    wl_list_remove(&server.request_cursor.link);
    wl_list_remove(&server.pointer_focus_change.link);
    wl_list_remove(&server.request_set_selection.link);
    wl_list_remove(&server.new_output.link);
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.wl_display);
    animation_free_snapshot(&server);
    free(window_usage_history);
    free(focus_cycle_snapshot);
    config_free_contents(&server.config);
    free(server.config_path);
    size_memory_free(&server);
    return 0;
}
