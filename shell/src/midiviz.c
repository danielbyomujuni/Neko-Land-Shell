// midiviz.c — piano-roll overlay for the context toolbar's visualizer.
//
// While direct monitoring an input whose MIDI device is linked in
// ~/.config/nekoland/midi-map.conf (Sound page), the device's notes are
// drawn as a scrolling piano roll over the audio spectrum: pitch maps to
// height, time scrolls right→left, held notes hug the right edge.
//
// toolbar.c decides WHEN this is active (midiviz_set_device) and WHERE
// it draws (midiviz_draw into the spectrum's region each frame).

#include "nekobar.h"

#include <alsa/asoundlib.h>
#include <string.h>

#define MV_MAX 256
#define MV_WINDOW_US 8e6 // the roll shows the last 8 seconds

typedef struct {
    guint8 pitch;
    guint8 vel;
    gint64 on_us;
    gint64 off_us; // 0 while held
} MvNote;

static MvNote notes[MV_MAX];
static int n_notes;
static snd_seq_t *mv_seq;
static guint mv_watch;
static char mv_dev[128];

static void note_on(guint8 pitch, guint8 vel) {
    if (n_notes == MV_MAX) { // drop the oldest
        memmove(notes, notes + 1, sizeof(MvNote) * (MV_MAX - 1));
        n_notes--;
    }
    notes[n_notes++] = (MvNote){pitch, vel, g_get_monotonic_time(), 0};
}

static void note_off(guint8 pitch) {
    for (int i = n_notes - 1; i >= 0; i--) {
        if (notes[i].pitch == pitch && notes[i].off_us == 0) {
            notes[i].off_us = g_get_monotonic_time();
            return;
        }
    }
}

static gboolean mv_io(GIOChannel *ch, GIOCondition cond, gpointer data) {
    (void)ch;
    (void)cond;
    (void)data;
    snd_seq_event_t *ev;
    while (mv_seq && snd_seq_event_input(mv_seq, &ev) >= 0) {
        switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            if (ev->data.note.velocity > 0)
                note_on(ev->data.note.note, ev->data.note.velocity);
            else // running-status note-off
                note_off(ev->data.note.note);
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            note_off(ev->data.note.note);
            break;
        case SND_SEQ_EVENT_NOTE: // combined on+off (duration form)
            note_on(ev->data.note.note, ev->data.note.velocity);
            note_off(ev->data.note.note);
            break;
        case SND_SEQ_EVENT_CONTROLLER:
            // all-notes-off / all-sound-off release everything held
            if (ev->data.control.param == 123 ||
                ev->data.control.param == 120)
                for (int i = 0; i < n_notes; i++)
                    if (notes[i].off_us == 0)
                        notes[i].off_us = g_get_monotonic_time();
            break;
        default:
            break;
        }
        if (snd_seq_event_input_pending(mv_seq, 0) <= 0)
            break;
    }
    return G_SOURCE_CONTINUE;
}

static void mv_close(void) {
    if (mv_watch) {
        g_source_remove(mv_watch);
        mv_watch = 0;
    }
    if (mv_seq) {
        snd_seq_close(mv_seq);
        mv_seq = NULL;
    }
    n_notes = 0;
    mv_dev[0] = '\0';
}

void midiviz_set_device(const char *dev) {
    if (!dev || !*dev) {
        mv_close();
        return;
    }
    if (mv_seq && g_str_equal(dev, mv_dev))
        return; // already listening to this one
    mv_close();
    if (snd_seq_open(&mv_seq, "default", SND_SEQ_OPEN_INPUT,
                     SND_SEQ_NONBLOCK) < 0) {
        mv_seq = NULL;
        return;
    }
    snd_seq_set_client_name(mv_seq, "nekobar-midiviz");
    int port = snd_seq_create_simple_port(
        mv_seq, "input",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_APPLICATION);
    // subscribe only the linked device's readable ports
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(mv_seq, cinfo) >= 0) {
        if (!g_str_equal(snd_seq_client_info_get_name(cinfo), dev))
            continue;
        int client = snd_seq_client_info_get_client(cinfo);
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(mv_seq, pinfo) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & SND_SEQ_PORT_CAP_READ) &&
                (caps & SND_SEQ_PORT_CAP_SUBS_READ))
                snd_seq_connect_from(mv_seq, port, client,
                                     snd_seq_port_info_get_port(pinfo));
        }
    }
    struct pollfd pfd;
    if (snd_seq_poll_descriptors(mv_seq, &pfd, 1, POLLIN) == 1) {
        GIOChannel *ch = g_io_channel_unix_new(pfd.fd);
        mv_watch = g_io_add_watch(ch, G_IO_IN, mv_io, NULL);
        g_io_channel_unref(ch);
    }
    g_strlcpy(mv_dev, dev, sizeof(mv_dev));
}

gboolean midiviz_active(void) {
    return mv_seq != NULL;
}

void midiviz_draw(cairo_t *cr, double x, double y, double w, double h) {
    gint64 now = g_get_monotonic_time();
    // stuck-note guard: a "held" note whose note-off was lost (noise on
    // the MIDI line, device quirk) would sit lit forever — release it
    // after 30s; real drones just show a 30s bar and fade like any note
    for (int i = 0; i < n_notes; i++)
        if (notes[i].off_us == 0 && now - notes[i].on_us > (gint64)30e6)
            notes[i].off_us = now;
    // prune notes that scrolled out
    int k = 0;
    for (int i = 0; i < n_notes; i++)
        if (!(notes[i].off_us &&
              now - notes[i].off_us > (gint64)MV_WINDOW_US))
            notes[k++] = notes[i];
    n_notes = k;
    if (!n_notes)
        return;

    // pitch window: the played range, padded, at least two octaves
    int lo = 127, hi = 0;
    for (int i = 0; i < n_notes; i++) {
        lo = MIN(lo, notes[i].pitch);
        hi = MAX(hi, notes[i].pitch);
    }
    int span = hi - lo;
    if (span < 24) {
        int pad = (24 - span) / 2;
        lo = MAX(0, lo - pad);
        hi = MIN(127, lo + 24);
    } else {
        lo = MAX(0, lo - 2);
        hi = MIN(127, hi + 2);
    }
    double rows = hi - lo + 1;
    double rh = MAX(2.0, h / rows * 0.8);

    for (int i = 0; i < n_notes; i++) {
        MvNote *nt = &notes[i];
        double x2 = nt->off_us
                        ? x + w - (now - nt->off_us) / MV_WINDOW_US * w
                        : x + w;
        double x1 = x + w - (now - nt->on_us) / MV_WINDOW_US * w;
        if (x2 <= x)
            continue;
        x1 = MAX(x1, x);
        double ny =
            y + h - ((nt->pitch - lo + 0.5) / rows) * h - rh / 2;
        double r = MIN(rh / 2, (x2 - x1) / 2);
        // maroon capsules, held notes fully lit
        double a = nt->off_us ? 0.55 : 0.95;
        cairo_set_source_rgba(cr, 0xF3 / 255.0, 0x8B / 255.0,
                              0xA8 / 255.0, a);
        cairo_new_sub_path(cr);
        cairo_arc(cr, x2 - r, ny + r, r, -G_PI / 2, G_PI / 2);
        cairo_arc(cr, x1 + r, ny + rh - r, r, G_PI / 2, 3 * G_PI / 2);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}
