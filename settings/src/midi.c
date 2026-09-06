// midi.c — "MIDI control" section of the Sound page.
//
// Every (non-hidden) audio input is listed with an inline dropdown of
// the connected MIDI devices; picking one links them. Any control (CC)
// turned on the linked device drives that input's volume, applied
// system-wide by nekoland-midid. Bindings live in
// ~/.config/nekoland/midi-map.conf as [<source>] device=<client name>.

#include "app.h"

#include <alsa/asoundlib.h>
#include <json-glib/json-glib.h>
#include <string.h>

static GPtrArray *midi_devs; // char* — connected MIDI client names
static gboolean mb_building; // guard: programmatic dropdown selection

static char *map_path(void) {
    return g_build_filename(g_get_user_config_dir(), "nekoland",
                            "midi-map.conf", NULL);
}

// the daemon replaces a running instance by itself (pidfile), so a plain
// respawn is a reload
static void midid_restart(void) {
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (!exe)
        return;
    char *dir = g_path_get_dirname(exe);
    char *bin = g_build_filename(dir, "nekoland-midid", NULL);
    char *argv[] = {bin, NULL};
    g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL,
                  NULL);
    g_free(bin);
    g_free(dir);
    g_free(exe);
}

// connected MIDI devices = sequencer clients with a readable port
static void midi_devs_load(void) {
    if (midi_devs)
        g_ptr_array_free(midi_devs, TRUE);
    midi_devs = g_ptr_array_new_with_free_func(g_free);
    snd_seq_t *seq;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT,
                     SND_SEQ_NONBLOCK) < 0)
        return;
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        const char *name = snd_seq_client_info_get_name(cinfo);
        if (client == snd_seq_client_id(seq) ||
            client == SND_SEQ_CLIENT_SYSTEM ||
            g_str_equal(name, "Midi Through") ||
            g_str_equal(name, "nekoland-midid"))
            continue;
        gboolean readable = FALSE;
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & SND_SEQ_PORT_CAP_READ) &&
                (caps & SND_SEQ_PORT_CAP_SUBS_READ))
                readable = TRUE;
        }
        if (readable)
            g_ptr_array_add(midi_devs, g_strdup(name));
    }
    snd_seq_close(seq);
}

// hidden inputs (Sound → Advanced) stay out of this list too
static GHashTable *hidden_load_set(void) {
    GHashTable *set =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *path = g_build_filename(g_get_user_config_dir(), "nekoland",
                                  "hidden-audio.conf", NULL);
    char *data = NULL;
    if (g_file_get_contents(path, &data, NULL, NULL)) {
        char **lines = g_strsplit(data, "\n", -1);
        for (char **l = lines; *l; l++) {
            g_strstrip(*l);
            if (**l)
                g_hash_table_add(set, g_strdup(*l));
        }
        g_strfreev(lines);
        g_free(data);
    }
    g_free(path);
    return set;
}

static char *bound_device_for(const char *source) {
    char *path = map_path();
    GKeyFile *kf = g_key_file_new();
    char *dev = NULL;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
        dev = g_key_file_get_string(kf, source, "device", NULL);
    g_key_file_free(kf);
    g_free(path);
    return dev;
}

static void binding_set(const char *source, const char *device) {
    char *path = map_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    if (device)
        g_key_file_set_string(kf, source, "device", device);
    else
        g_key_file_remove_group(kf, source, NULL);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
    midid_restart();
}

static void src_free(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static void on_dev_selected(GObject *drop, GParamSpec *spec, gpointer data) {
    (void)spec;
    if (mb_building)
        return;
    const char *source = data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(drop));
    if (sel == 0 || sel == GTK_INVALID_LIST_POSITION)
        binding_set(source, NULL); // "None"
    else if (sel - 1 < midi_devs->len)
        binding_set(source, g_ptr_array_index(midi_devs, sel - 1));
}

// ---- section ----

GtkWidget *midi_section_new(void) {
    midi_devs_load();

    GtkWidget *sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    GtkWidget *lbl = gtk_label_new("MIDI CONTROL");
    gtk_widget_add_css_class(lbl, "section-label");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_append(GTK_BOX(sec), lbl);

    // card matching the OUTPUT/INPUT device lists: zero own padding,
    // padded rows separated by hairlines
    GtkWidget *cardw = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(cardw, "nk-card");

    // dropdown model: None + connected devices
    const char **strv = g_new0(const char *, midi_devs->len + 2);
    strv[0] = "None";
    for (guint i = 0; i < midi_devs->len; i++)
        strv[i + 1] = g_ptr_array_index(midi_devs, i);

    GHashTable *hidden = hidden_load_set();
    char *out = NULL;
    g_spawn_command_line_sync("pactl --format=json list sources", &out,
                              NULL, NULL, NULL);
    gboolean any = FALSE;
    mb_building = TRUE;
    if (out) {
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, out, -1, NULL)) {
            JsonArray *arr = json_node_get_array(json_parser_get_root(p));
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *o = json_array_get_object_element(arr, i);
                const char *n = json_object_get_string_member(o, "name");
                if (!n || !g_str_has_prefix(n, "alsa_input.") ||
                    g_hash_table_contains(hidden, n))
                    continue;
                const char *desc =
                    json_object_get_string_member_with_default(
                        o, "description", n);
                if (any) {
                    GtkWidget *sep =
                        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
                    gtk_widget_add_css_class(sep, "nk-card-sep");
                    gtk_box_append(GTK_BOX(cardw), sep);
                }
                GtkWidget *row =
                    gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
                gtk_widget_add_css_class(row, "form-row");
                gtk_widget_add_css_class(row, "midi-row");
                GtkWidget *l = gtk_label_new(desc);
                gtk_label_set_xalign(GTK_LABEL(l), 0.0);
                gtk_label_set_ellipsize(GTK_LABEL(l),
                                        PANGO_ELLIPSIZE_END);
                gtk_widget_set_hexpand(l, TRUE);
                gtk_box_append(GTK_BOX(row), l);
                GtkWidget *drop = gtk_drop_down_new_from_strings(strv);
                gtk_widget_set_valign(drop, GTK_ALIGN_CENTER);
                // preselect the saved binding
                char *bound = bound_device_for(n);
                if (bound) {
                    for (guint d = 0; d < midi_devs->len; d++)
                        if (g_str_equal(
                                g_ptr_array_index(midi_devs, d), bound))
                            gtk_drop_down_set_selected(
                                GTK_DROP_DOWN(drop), d + 1);
                    g_free(bound);
                }
                g_signal_connect_data(drop, "notify::selected",
                                      G_CALLBACK(on_dev_selected),
                                      g_strdup(n), src_free, 0);
                gtk_box_append(GTK_BOX(row), drop);
                gtk_box_append(GTK_BOX(cardw), row);
                any = TRUE;
            }
        }
        g_object_unref(p);
        g_free(out);
    }
    mb_building = FALSE;
    g_free(strv);
    g_hash_table_destroy(hidden);

    if (!any) {
        GtkWidget *hint = gtk_label_new("No audio inputs found.");
        gtk_widget_add_css_class(hint, "hint-label");
        gtk_widget_add_css_class(hint, "form-row");
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
        gtk_box_append(GTK_BOX(cardw), hint);
    }

    gtk_box_append(GTK_BOX(sec), cardw);

    // footnote under the card, like the page's other hints
    GtkWidget *hint = gtk_label_new(
        "A linked device's knobs and faders drive that input's volume.");
    gtk_widget_add_css_class(hint, "hint-label");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_box_append(GTK_BOX(sec), hint);

    return sec;
}
