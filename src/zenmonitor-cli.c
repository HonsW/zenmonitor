#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <ncurses.h>
#include "zenmonitor.h"
#include "zenpower.h"
#include "msr.h"
#include "os.h"

gboolean display_coreid = 0;
static gdouble delay = 0.5;
static gchar *file = "";
static gchar *average_spec = NULL;
static gchar *snapshot_path = NULL;
static gchar *sensor_spec = NULL;
static gchar **sensor_filter = NULL;   // NULL => every sensor is selected
static gint refresh_in_place = 0;
static gint output_once = 0;
static gint daemon_mode = 0;

static FILE *csv = NULL;          // streaming CSV log (--file), NULL when off
static guint n_selected = 0;      // sensors passing the --sensors filter
static volatile sig_atomic_t stop_requested = 0;

static GOptionEntry options[] = {
    {"file", 'f', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &file,
     "Append readings to a CSV file (one row per refresh, crash-safe)", "FILE"},
    {"delay", 'd', G_OPTION_FLAG_NONE, G_OPTION_ARG_DOUBLE, &delay,
     "Interval between refreshes in seconds (default 0.5)", "SECONDS"},
    {"coreid", 'c', 0, G_OPTION_ARG_NONE, &display_coreid,
     "Display core_id instead of core index", NULL},
    {"average", 'a', 0, G_OPTION_ARG_STRING, &average_spec,
     "Rolling-average windows for --daemon output, e.g. 30s,1m,5m", "WINDOWS"},
    {"sensors", 'S', 0, G_OPTION_ARG_STRING, &sensor_spec,
     "Only handle sensors whose label contains one of these comma-separated substrings (case-insensitive)", "SUBSTRINGS"},
    {"daemon", 'D', 0, G_OPTION_ARG_NONE, &daemon_mode,
     "Run resident, writing a snapshot file each interval (keeps history for averages)", NULL},
    {"snapshot", 's', 0, G_OPTION_ARG_STRING, &snapshot_path,
     "Snapshot file path for --daemon (default $XDG_RUNTIME_DIR/zenmonitor.snapshot)", "FILE"},
    {"refresh-in-place", 'r', 0, G_OPTION_ARG_NONE, &refresh_in_place,
     "Redraw the readings in place instead of scrolling", NULL},
    {"output-once", 'o', 0, G_OPTION_ARG_NONE, &output_once,
     "Output CPU information once and quit", NULL},
    {NULL}};

static SensorSource sensor_sources[] = {
    {
        "zenpower",
        zenpower_init, zenpower_get_sensors, zenpower_update, zenpower_clear_minmax,
        FALSE, NULL
    },
    {
        "msr",
        msr_init, msr_get_sensors, msr_update, msr_clear_minmax,
        FALSE, NULL
    },
    {
        "os",
        os_init, os_get_sensors, os_update, os_clear_minmax,
        FALSE, NULL
    },
    {
        NULL
    }
};

// Async-signal-safe: just request a clean shutdown. The ncurses teardown,
// CSV close, and daemon snapshot removal happen back in the main loop.
static void request_stop(int signum) {
    (void)signum;
    stop_requested = 1;
}

#define sensor_selected(label) str_filter_match(sensor_filter, (label))

static void init_sensors(void) {
    GSList *sensor;
    SensorSource *source;
    const SensorInit *data;

    for (source = sensor_sources; source->drv; source++) {
        if (source->func_init()) {
            source->sensors = source->func_get_sensors();
            if (source->sensors != NULL) {
                guint selected = 0;
                sensor = source->sensors;
                while (sensor) {
                    data = (SensorInit *)sensor->data;
                    if (sensor_selected(data->label))
                        selected++;
                    sensor = sensor->next;
                }
                // Skip the whole source at update time when nothing matched,
                // avoiding its per-tick sensor reads (e.g. the MSR preads).
                source->enabled = (selected > 0);
                n_selected += selected;
            }
        }
    }
}

// Open the CSV log and write the header. Rows are appended one per refresh in
// update_data() and flushed immediately, so the file is valid mid-run
// (tail -f works) and survives a crash up to the last row - unlike the old
// dump-at-exit design, which also held every sample in memory.
static gboolean csv_open(const gchar *path) {
    SensorSource *source;
    GSList *node;
    const SensorInit *data;

    csv = fopen(path, "w");
    if (!csv) {
        fprintf(stderr, "zenmonitor-cli: cannot open '%s': %s\n",
                path, g_strerror(errno));
        return FALSE;
    }

    // Column order matches the iteration order used in update_data().
    fprintf(csv, "time(epoch)");
    for (source = sensor_sources; source->drv; source++) {
        if (!source->enabled)
            continue;
        for (node = source->sensors; node; node = node->next) {
            data = (SensorInit *)node->data;
            if (sensor_selected(data->label))
                fprintf(csv, ",%s", data->label);
        }
    }
    fprintf(csv, "\n");
    fflush(csv);
    return TRUE;
}

static void update_data(void) {
    SensorSource *source;
    GSList *node;
    const SensorInit *sensorData;
    int row = 1; // ncurses uses 1-based row indexing

    if (csv) {
        struct timespec ts;
        timespec_get(&ts, TIME_UTC);
        fprintf(csv, "%ld.%.9ld", (long)ts.tv_sec, (long)ts.tv_nsec);
    }

    for (source = sensor_sources; source->drv; source++) {
        if (!source->enabled)
            continue;

        source->func_update();
        if (!source->sensors)
            continue;

        node = source->sensors;
        while (node) {
            sensorData = (SensorInit *)node->data;
            if (!sensor_selected(sensorData->label)) {
                node = node->next;
                continue;
            }
            if (csv)
                fprintf(csv, ",%f", *sensorData->value);

            if (refresh_in_place) {
                mvprintw(row++, 0, "%s\t%f", sensorData->label, *sensorData->value);
            } else {
                printf("%s\t%f\n", sensorData->label, *sensorData->value);
            }
            node = node->next;
        }
    }

    if (csv) {
        fprintf(csv, "\n");
        fflush(csv);
    }

    if (refresh_in_place)
        refresh();
    else
        printf("\v");
}

static void start_watching(void) {
    while (!stop_requested) {
        update_data();
        if (output_once)
            break;
        usleep(delay * 1000 * 1000);
    }
}

// ---- Daemon mode ----------------------------------------------------------
// Keeps rolling-average history in memory and rewrites a snapshot file each
// interval, so short-lived readers (e.g. an xfce4-genmon panel) can display a
// trailing average without holding any history themselves.

static gchar *default_snapshot_path(void) {
    const gchar *runtime = g_getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime)
        return g_build_filename(runtime, "zenmonitor.snapshot", NULL);
    return g_strdup("/tmp/zenmonitor.snapshot");
}

// Write the snapshot atomically: fill a temp file, then rename over the target.
static void write_snapshot(const gchar *path, const AvgWindows *avg, AvgSeries *series) {
    SensorSource *source;
    GSList *node;
    const SensorInit *sensorData;
    gchar *tmp;
    FILE *out;
    guint idx = 0, k;

    tmp = g_strdup_printf("%s.tmp", path);
    out = fopen(tmp, "w");
    if (!out) {
        fprintf(stderr, "zenmonitor-cli: cannot open '%s': %s\n", tmp, g_strerror(errno));
        g_free(tmp);
        return;
    }

    fprintf(out, "# updated %ld\n", (long)time(NULL));
    fprintf(out, "# sensor\tvalue");
    for (k = 0; k < avg->count; k++)
        fprintf(out, "\t%s", avg->titles[k]);
    fprintf(out, "\n");

    for (source = sensor_sources; source->drv; source++) {
        if (!source->enabled)
            continue;

        source->func_update();
        if (!source->sensors)
            continue;

        node = source->sensors;
        while (node) {
            sensorData = (SensorInit *)node->data;
            if (!sensor_selected(sensorData->label)) {
                node = node->next;
                continue;
            }
            float value = *sensorData->value;

            avg_series_push(&series[idx], avg, value);

            fprintf(out, "%s\t%f", sensorData->label, value);
            for (k = 0; k < avg->count; k++) {
                if (series[idx].valid)
                    fprintf(out, "\t%f", series[idx].avg[k]);
                else
                    fprintf(out, "\t");
            }
            fprintf(out, "\n");

            idx++;
            node = node->next;
        }
    }

    fclose(out);

    if (rename(tmp, path) != 0)
        fprintf(stderr, "zenmonitor-cli: cannot update '%s': %s\n", path, g_strerror(errno));

    g_free(tmp);
}

static int run_daemon(void) {
    AvgWindows *avg;
    AvgSeries *series;
    guint n_sensors, i;
    guint interval_ms;

    if (!snapshot_path)
        snapshot_path = default_snapshot_path();

    interval_ms = (guint)(delay * 1000.0 + 0.5);
    avg = avg_windows_parse(average_spec, interval_ms);

    n_sensors = n_selected;
    series = g_new0(AvgSeries, n_sensors ? n_sensors : 1);
    for (i = 0; i < n_sensors; i++)
        avg_series_init(&series[i], avg);

    fprintf(stderr, "zenmonitor-cli: daemon writing '%s' every %.3gs\n",
            snapshot_path, delay);

    while (!stop_requested) {
        write_snapshot(snapshot_path, avg, series);
        if (output_once)
            break;
        usleep(delay * 1000 * 1000);
    }

    if (!output_once)
        unlink(snapshot_path);

    for (i = 0; i < n_sensors; i++)
        avg_series_free(&series[i]);
    g_free(series);
    avg_windows_free(avg);
    return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
    GError *error = NULL;
    GOptionContext *context;
    gboolean write_csv;
    int ret;

    context = g_option_context_new("- Zenmonitor command line interface");
    g_option_context_add_main_entries(context, options, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_print("option parsing failed: %s\n", error->message);
        exit(1);
    }

    write_csv = (strcmp(file, "") != 0);
    sensor_filter = str_filter_parse(sensor_spec);

    // Guard against a zero/negative interval: 0 would busy-loop, and a
    // negative value wraps through the usleep() and window-math casts.
    if (delay < 0.05) {
        fprintf(stderr, "zenmonitor-cli: --delay too small, using 0.05s\n");
        delay = 0.05;
    }

    // Handle Ctrl-C/termination ourselves so ncurses is torn down, the CSV is
    // closed, and the daemon snapshot is cleaned up.
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    init_sensors();

    if (daemon_mode) {
        if (write_csv)
            fprintf(stderr, "zenmonitor-cli: --file is ignored in --daemon mode\n");
        ret = run_daemon();
        return ret;
    }

    // Fail fast if the log can't be opened, instead of sampling for hours and
    // discovering it at exit.
    if (write_csv && !csv_open(file))
        return EXIT_FAILURE;

    if (refresh_in_place) {
        initscr();
        curs_set(0);
    }

    start_watching();

    if (refresh_in_place)
        endwin();

    if (csv)
        fclose(csv);

    return EXIT_SUCCESS;
}
