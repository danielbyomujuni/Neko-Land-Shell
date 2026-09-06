// nekoland-midid — tiny ALSA-sequencer daemon applying the MIDI→input
// bindings from ~/.config/nekoland/midi-map.conf system-wide.
//
// A binding links a MIDI DEVICE (sequencer client name) to a pipewire
// source: any control (CC) turned on that device sets the linked audio
// input's volume (0..127 → 0..100%). Config groups are source names
// with a device= key.
// The Sound page edits the config and respawns this daemon; a pidfile in
// $XDG_RUNTIME_DIR makes each new invocation replace the previous one.
//
// All readable MIDI ports are subscribed, and the System:Announce port is
// watched so newly plugged controllers connect automatically.

#include <alsa/asoundlib.h>
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *device; // sequencer client name
    char *source;
    int pending;   // latest value while a debounce is in flight (-1 none)
    guint timer;
} Bind;

static GPtrArray *binds; // Bind*
static snd_seq_t *seq;
static int seq_port;

static char *pidfile_path(void) {
    return g_build_filename(g_get_user_runtime_dir(), "nekoland-midid.pid",
                            NULL);
}

// kill any previous instance, then claim the pidfile (lcdd's pattern)
static void singleton_replace(void) {
    char *pf = pidfile_path();
    char *content = NULL;
    if (g_file_get_contents(pf, &content, NULL, NULL)) {
        int old = atoi(content);
        g_free(content);
        if (old > 0 && old != getpid()) {
            char *comm_path = g_strdup_printf("/proc/%d/comm", old);
            char *comm = NULL;
            g_file_get_contents(comm_path, &comm, NULL, NULL);
            g_free(comm_path);
            if (comm && g_str_has_prefix(comm, "nekoland-midi")) {
                kill(old, SIGTERM);
                for (int i = 0; i < 20 && kill(old, 0) == 0; i++)
                    g_usleep(50 * 1000);
            }
            g_free(comm);
        }
    }
    char pid[16];
    g_snprintf(pid, sizeof(pid), "%d\n", getpid());
    g_file_set_contents(pf, pid, -1, NULL);
    g_free(pf);
}

static void binds_load(void) {
    binds = g_ptr_array_new();
    char *path = g_build_filename(g_get_user_config_dir(), "nekoland",
                                  "midi-map.conf", NULL);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        gsize n = 0;
        char **groups = g_key_file_get_groups(kf, &n); // groups = sources
        for (gsize i = 0; i < n; i++) {
            char *dev =
                g_key_file_get_string(kf, groups[i], "device", NULL);
            if (!dev)
                continue;
            Bind *b = g_new0(Bind, 1);
            b->device = dev;
            b->source = g_strdup(groups[i]);
            b->pending = -1;
            g_ptr_array_add(binds, b);
        }
        g_strfreev(groups);
    }
    g_key_file_free(kf);
    g_free(path);
}

static void apply_volume(Bind *b, int val) {
    char *argv[] = {"pactl", "set-source-volume", b->source, NULL, NULL};
    char pct[16];
    g_snprintf(pct, sizeof(pct), "%d%%", val * 100 / 127);
    argv[3] = pct;
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
                  NULL);
}

// knobs stream events far faster than pactl calls are worth: apply now,
// then coalesce further turns for 40ms and apply the newest value
static gboolean debounce_cb(gpointer data) {
    Bind *b = data;
    b->timer = 0;
    if (b->pending >= 0) {
        apply_volume(b, b->pending);
        b->pending = -1;
    }
    return G_SOURCE_REMOVE;
}

// resolve a sender client id to its name (tiny 1-entry cache: events
// arrive in bursts from the same device)
static const char *client_name(int client) {
    static int last_id = -1;
    static char last_name[128];
    if (client == last_id)
        return last_name;
    snd_seq_client_info_t *ci;
    snd_seq_client_info_alloca(&ci);
    if (snd_seq_get_any_client_info(seq, client, ci) < 0)
        return "";
    g_strlcpy(last_name, snd_seq_client_info_get_name(ci),
              sizeof(last_name));
    last_id = client;
    return last_name;
}

static void handle_cc(int sender_client, int val) {
    const char *dev = client_name(sender_client);
    for (guint i = 0; i < binds->len; i++) {
        Bind *b = g_ptr_array_index(binds, i);
        if (!g_str_equal(b->device, dev))
            continue;
        if (b->timer) {
            b->pending = val;
        } else {
            apply_volume(b, val);
            b->timer = g_timeout_add(40, debounce_cb, b);
        }
    }
}

// subscribe every readable port (skip our own client and System)
static void connect_port(int client, int port) {
    if (client == snd_seq_client_id(seq) || client == SND_SEQ_CLIENT_SYSTEM)
        return;
    snd_seq_connect_from(seq, seq_port, client, port);
}

static void connect_all(void) {
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & SND_SEQ_PORT_CAP_READ) &&
                (caps & SND_SEQ_PORT_CAP_SUBS_READ))
                connect_port(client, snd_seq_port_info_get_port(pinfo));
        }
    }
}

static gboolean seq_io(GIOChannel *ch, GIOCondition cond, gpointer data) {
    (void)ch;
    (void)cond;
    (void)data;
    snd_seq_event_t *ev;
    while (snd_seq_event_input(seq, &ev) >= 0) {
        if (ev->type == SND_SEQ_EVENT_CONTROLLER)
            handle_cc(ev->source.client, ev->data.control.value);
        else if (ev->type == SND_SEQ_EVENT_PORT_START)
            connect_port(ev->data.addr.client, ev->data.addr.port);
        if (snd_seq_event_input_pending(seq, 0) <= 0)
            break;
    }
    return G_SOURCE_CONTINUE;
}

int main(void) {
    singleton_replace();
    binds_load();
    fprintf(stderr, "midid: %u binding(s)\n", binds->len);

    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) <
        0) {
        fprintf(stderr, "midid: cannot open alsa sequencer\n");
        return 1;
    }
    snd_seq_set_client_name(seq, "nekoland-midid");
    seq_port = snd_seq_create_simple_port(
        seq, "input",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_APPLICATION);
    // announce port → auto-connect controllers plugged in later
    snd_seq_connect_from(seq, seq_port, SND_SEQ_CLIENT_SYSTEM,
                         SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    connect_all();

    int npfd = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd *pfds = g_new0(struct pollfd, npfd);
    snd_seq_poll_descriptors(seq, pfds, npfd, POLLIN);
    for (int i = 0; i < npfd; i++) {
        GIOChannel *ch = g_io_channel_unix_new(pfds[i].fd);
        g_io_add_watch(ch, G_IO_IN, seq_io, NULL);
        g_io_channel_unref(ch);
    }
    g_free(pfds);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
