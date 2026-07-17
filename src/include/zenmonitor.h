#ifndef __ZENMONITOR_ZENMONITOR_H__
#define __ZENMONITOR_ZENMONITOR_H__

#include <time.h>
#include <glib.h>

#define ERROR_VALUE -999.0
#define VERSION "1.5.0"

typedef struct
{
    gchar *label;
    gchar *hint;
    float *value;
    float *min;
    float *max;
    const gchar *printf_format;
}
SensorInit;

typedef struct {
    const gchar *drv;
    gboolean  (*func_init)(void);
    GSList* (*func_get_sensors)(void);
    void (*func_update)(void);
    void (*func_clear_minmax)(void);
    gboolean enabled;
    GSList *sensors;
} SensorSource;

typedef struct {
    GPtrArray *labels;
    GPtrArray *data;
    GArray *time;
} SensorDataStore;

// Rolling-average configuration, shared by the GUI columns and the CLI daemon.
typedef struct {
    gchar **titles;   // count entries: column/label heading, e.g. "Avg 1m"
    guint *samples;   // count entries: window length expressed in samples
    guint count;      // number of windows (0 = averaging disabled)
    guint cap;        // ring capacity = largest window in samples
} AvgWindows;

// Per-series rolling state (one per GUI row / per CLI sensor).
typedef struct {
    float *buf;       // ring buffer of the last AvgWindows.cap samples
    gdouble *sum;     // running sum per window
    gdouble *avg;     // last computed average per window
    guint n;          // number of valid samples pushed
    gboolean valid;   // TRUE once at least one sample is present
} AvgSeries;

SensorInit* sensor_init_new(void);
void sensor_init_free(SensorInit *s);
gboolean check_zen(void);
gchar *cpu_model(void);
guint get_core_count(void);

SensorDataStore* sensor_data_store_new(void);
void sensor_data_store_add_entry(SensorDataStore *store, gchar *entry);
gint sensor_data_store_drop_entry(SensorDataStore *store, gchar *entry);
void sensor_data_store_keep_time(SensorDataStore *store);
gint sensor_data_store_add_data(SensorDataStore *store, gchar *entry, float data);
void sensor_data_store_free(SensorDataStore *store);

AvgWindows* avg_windows_parse(const gchar *spec, guint interval_ms);
void avg_windows_free(AvgWindows *w);
void avg_series_init(AvgSeries *s, const AvgWindows *w);
void avg_series_free(AvgSeries *s);
void avg_series_push(AvgSeries *s, const AvgWindows *w, float value);

gchar** str_filter_parse(const gchar *spec);
gboolean str_filter_match(gchar * const *filter, const gchar *text);
void str_filter_free(gchar **filter);

extern gboolean display_coreid;

#endif /* __ZENMONITOR_ZENMONITOR_H__ */
