#define _GNU_SOURCE     /* for strcasestr */
#include <cpuid.h>
#include <string.h>
#include <time.h>
#include "zenmonitor.h"

#define AMD_STRING "AuthenticAMD"
#define ZEN_FAMILY 0x17
#define ZEN3_FAMILY 0x19

// AMD PPR = https://www.amd.com/system/files/TechDocs/54945_PPR_Family_17h_Models_00h-0Fh.pdf

gboolean check_zen(void) {
    guint32 eax = 0, ebx = 0, ecx = 0, edx = 0, ext_family;
    char vendor[13];

    __get_cpuid(0, &eax, &ebx, &ecx, &edx);

    memcpy(vendor, &ebx, 4);
    memcpy(vendor+4, &edx, 4);
    memcpy(vendor+8, &ecx, 4);
    vendor[12] = 0;

    if (strcmp(vendor, AMD_STRING) != 0){
        return FALSE;
    }

    __get_cpuid(1, &eax, &ebx, &ecx, &edx);

    ext_family = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
    if (ext_family != ZEN_FAMILY && ext_family != ZEN3_FAMILY){
        return FALSE;
    }

    return TRUE;
}

gchar *cpu_model(void) {
    guint32 eax = 0, ebx = 0, ecx = 0, edx = 0;
    char model[49];

    // AMD PPR: page 65-68 - CPUID_Fn80000002_EAX-CPUID_Fn80000004_EDX
    __get_cpuid(0x80000002, &eax, &ebx, &ecx, &edx);
    memcpy(model, &eax, 4);
    memcpy(model+4, &ebx, 4);
    memcpy(model+8, &ecx, 4);
    memcpy(model+12, &edx, 4);

    __get_cpuid(0x80000003, &eax, &ebx, &ecx, &edx);
    memcpy(model+16, &eax, 4);
    memcpy(model+20, &ebx, 4);
    memcpy(model+24, &ecx, 4);
    memcpy(model+28, &edx, 4);

    __get_cpuid(0x80000004, &eax, &ebx, &ecx, &edx);
    memcpy(model+32, &eax, 4);
    memcpy(model+36, &ebx, 4);
    memcpy(model+40, &ecx, 4);
    memcpy(model+44, &edx, 4);

    model[48] = 0;
    return g_strdup(g_strchomp(model));
}

guint get_core_count(void) {
    guint eax = 0, ebx = 0, ecx = 0, edx = 0;
    guint logical_cpus, threads_per_core;

    // AMD PPR: page 57 - CPUID_Fn00000001_EBX
    __get_cpuid(1, &eax, &ebx, &ecx, &edx);
    logical_cpus = (ebx >> 16) & 0xFF;

    // AMD PPR: page 82 - CPUID_Fn8000001E_EBX
    __get_cpuid(0x8000001E, &eax, &ebx, &ecx, &edx);
    // ThreadsPerCore is zero-based, so add 1 (always >= 1).
    threads_per_core = ((ebx >> 8) & 0xF) + 1;

    return logical_cpus / threads_per_core;
}

SensorInit *sensor_init_new(void) {
    return g_new0(SensorInit, 1);
}

void sensor_init_free(SensorInit *s) {
    if (s) {
        g_free(s->label);
        g_free(s->hint);
        g_free(s);
    }
}

// Time-series store used by the CLI to accumulate readings for CSV export.
// labels: GPtrArray of gchar* sensor names (borrowed, owned by the sensors).
// data:   GPtrArray of GArray<float>, one series per label.
// time:   GArray<struct timespec>, one timestamp per sample row.
SensorDataStore *sensor_data_store_new(void) {
    SensorDataStore *ret;

    ret = g_new0(SensorDataStore, 1);
    ret->labels = g_ptr_array_new();
    ret->data = g_ptr_array_new();
    ret->time = g_array_new(FALSE, TRUE, sizeof(struct timespec));

    return ret;
}

void sensor_data_store_add_entry(SensorDataStore *store, gchar *entry) {
    GArray *data;
    data = g_array_new(TRUE, TRUE, sizeof(float));

    g_ptr_array_add(store->labels, entry);
    g_ptr_array_add(store->data, data);
}

gint sensor_data_store_drop_entry(SensorDataStore *store, gchar *entry) {
    guint index = 0;
    if (!g_ptr_array_find(store->labels, entry, &index))
        return 1;

    g_ptr_array_remove_index(store->labels, index);
    g_ptr_array_remove_index(store->data, index);

    return 0;
}

void sensor_data_store_keep_time(SensorDataStore *store) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    g_array_append_val(store->time, ts);
}

gint sensor_data_store_add_data(SensorDataStore *store, gchar *entry, float value) {
    guint index = 0;
    if (!g_ptr_array_find(store->labels, entry, &index))
        return 1;

    GArray *data = g_ptr_array_index(store->data, index);
    g_array_append_val(data, value);

    return 0;
}

void sensor_data_store_free(SensorDataStore *store) {
    if (store) {
        guint i;
        for (i = 0; i < store->data->len; i++)
            g_array_free(g_ptr_array_index(store->data, i), TRUE);

        g_array_free(store->time, TRUE);
        g_ptr_array_free(store->labels, TRUE);
        g_ptr_array_free(store->data, TRUE);
        g_free(store);
    }
}

// ---- Rolling averages -----------------------------------------------------
// A window given in seconds is converted to a sample count using the caller's
// sampling interval, so the same "5m" means 5 minutes regardless of how often
// the GUI or CLI polls. Each series keeps a ring buffer sized to the largest
// window plus a running sum per window, yielding an exact windowed mean in
// O(windows) per push with no re-summing.

static gboolean parse_window_seconds(const gchar *tok, gdouble *out_seconds) {
    gchar *end = NULL;
    gdouble val, mult;

    val = g_ascii_strtod(tok, &end);
    if (end == tok)
        return FALSE;

    while (*end == ' ')
        end++;

    switch (*end) {
        case '\0':
        case 's': case 'S': mult = 1.0;    break;
        case 'm': case 'M': mult = 60.0;   break;
        case 'h': case 'H': mult = 3600.0; break;
        default: return FALSE;
    }

    *out_seconds = val * mult;
    return TRUE;
}

AvgWindows *avg_windows_parse(const gchar *spec, guint interval_ms) {
    AvgWindows *w;
    gchar **tokens;
    guint i;

    w = g_new0(AvgWindows, 1);
    if (!spec || *spec == '\0' || interval_ms == 0)
        return w; // count == 0: averaging disabled

    tokens = g_strsplit(spec, ",", -1);
    w->titles = g_new0(gchar *, g_strv_length(tokens));
    w->samples = g_new0(guint, g_strv_length(tokens));

    for (i = 0; tokens[i]; i++) {
        gchar *tok = g_strstrip(tokens[i]);
        gdouble seconds;
        guint samples;

        if (*tok == '\0')
            continue;

        if (!parse_window_seconds(tok, &seconds) || seconds <= 0) {
            g_printerr("zenmonitor: ignoring invalid average window '%s'\n", tok);
            continue;
        }

        samples = (guint)((seconds * 1000.0) / interval_ms + 0.5);
        if (samples < 1)
            samples = 1;

        w->titles[w->count] = g_strdup_printf("Avg %s", tok);
        w->samples[w->count] = samples;
        if (samples > w->cap)
            w->cap = samples;
        w->count++;
    }

    g_strfreev(tokens);
    return w;
}

void avg_windows_free(AvgWindows *w) {
    guint i;
    if (!w)
        return;
    for (i = 0; i < w->count; i++)
        g_free(w->titles[i]);
    g_free(w->titles);
    g_free(w->samples);
    g_free(w);
}

void avg_series_init(AvgSeries *s, const AvgWindows *w) {
    s->n = 0;
    s->valid = FALSE;
    if (w->count == 0) {
        s->buf = NULL;
        s->sum = NULL;
        s->avg = NULL;
        return;
    }
    s->buf = g_new0(float, w->cap);
    s->sum = g_new0(gdouble, w->count);
    s->avg = g_new0(gdouble, w->count);
}

void avg_series_free(AvgSeries *s) {
    if (!s)
        return;
    g_free(s->buf);
    g_free(s->sum);
    g_free(s->avg);
    s->buf = NULL;
    s->sum = NULL;
    s->avg = NULL;
}

// ---- Substring filter -----------------------------------------------------
// Parse a comma-separated list of substrings into a NULL-terminated array
// (empty tokens dropped). Returns NULL for an empty/unset spec, meaning
// "match everything". Free with str_filter_free.

gchar **str_filter_parse(const gchar *spec) {
    gchar **raw;
    GPtrArray *arr;
    guint i;

    if (!spec || *spec == '\0')
        return NULL;

    raw = g_strsplit(spec, ",", -1);
    arr = g_ptr_array_new();
    for (i = 0; raw[i]; i++) {
        gchar *tok = g_strstrip(raw[i]);
        if (*tok)
            g_ptr_array_add(arr, g_strdup(tok));
    }
    g_strfreev(raw);

    if (arr->len == 0) {
        g_ptr_array_free(arr, TRUE);
        return NULL;
    }

    g_ptr_array_add(arr, NULL);
    return (gchar **)g_ptr_array_free(arr, FALSE);
}

gboolean str_filter_match(gchar * const *filter, const gchar *text) {
    guint i;
    if (!filter)
        return TRUE;
    for (i = 0; filter[i]; i++)
        if (strcasestr(text, filter[i]))
            return TRUE;
    return FALSE;
}

void str_filter_free(gchar **filter) {
    if (filter)
        g_strfreev(filter);
}

void avg_series_push(AvgSeries *s, const AvgWindows *w, float value) {
    guint k;

    // Skip error readings so a transient failure doesn't skew the window.
    if (w->count == 0 || value == ERROR_VALUE)
        return;

    for (k = 0; k < w->count; k++) {
        guint window = w->samples[k];
        guint denom;

        s->sum[k] += value;
        if (s->n >= window)
            s->sum[k] -= s->buf[(s->n - window) % w->cap];

        denom = (s->n + 1 < window) ? (s->n + 1) : window;
        s->avg[k] = s->sum[k] / denom;
    }

    s->buf[s->n % w->cap] = value;
    s->n++;
    s->valid = TRUE;
}
