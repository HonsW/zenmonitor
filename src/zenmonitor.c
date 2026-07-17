#include <gtk/gtk.h>
#include <stdlib.h>
#include "zenmonitor.h"
#include "zenpower.h"
#include "msr.h"
#include "os.h"
#include "gui.h"

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

gboolean display_coreid = 0;
static gchar *average_spec = NULL;
static gchar *average_only_spec = NULL;

static GOptionEntry options[] =
{
    { "coreid", 'c', 0, G_OPTION_ARG_NONE, &display_coreid, "Display core_id instead of core index", NULL },
    { "average", 'a', 0, G_OPTION_ARG_STRING, &average_spec, "Show rolling-average columns for the given comma-separated windows (e.g. 30s,1m,5m)", "WINDOWS" },
    { "average-only", 'A', 0, G_OPTION_ARG_STRING, &average_only_spec, "Only average sensors whose label contains one of these comma-separated substrings (e.g. power,temp)", "SUBSTRINGS" },
    { NULL }
};

int main (int argc, char *argv[])
{
    GError *error = NULL;
    GOptionContext *context;

    context = g_option_context_new ("- Zenmonitor display options");
    g_option_context_add_main_entries(context, options, NULL);
    g_option_context_add_group(context, gtk_get_option_group (TRUE));
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_print ("option parsing failed: %s\n", error->message);
        exit (1);
    }

    gui_set_averages(average_spec);
    gui_set_average_filter(average_only_spec);
    start_gui(sensor_sources);
}
