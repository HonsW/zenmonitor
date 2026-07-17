#include <cpuid.h>
#include <gtk/gtk.h>
#include "zenmonitor.h"
#include "gui.h"

// Sensor refresh cadence. Used both for the GTK timer and to translate the
// configured average windows (in seconds) into a number of samples.
#define REFRESH_INTERVAL_MS 200

GtkWidget *window;

static GtkTreeModel *model = NULL;
static guint timeout = 0;
static SensorSource *sensor_sources;
static const guint defaultHeight = 350;

// Fixed columns; any configured rolling-average columns follow at
// COLUMN_AVG_BASE .. COLUMN_AVG_BASE + avg->count - 1.
#define COLUMN_NAME     0
#define COLUMN_HINT     1
#define COLUMN_VALUE    2
#define COLUMN_MIN      3
#define COLUMN_MAX      4
#define COLUMN_AVG_BASE 5

// Rolling-average configuration (count == 0 by default -> no average columns).
static AvgWindows *avg = NULL;
static AvgSeries *series = NULL;   // one per tree row
static gboolean *row_avg = NULL;   // whether each row is averaged (--average-only)
static gchar **avg_filter = NULL;  // NULL => average every sensor
static guint n_rows = 0;

static guint avg_count(void) {
    return avg ? avg->count : 0;
}

// Parse a comma-separated list of window durations (e.g. "30s,1m,5m").
// Passing NULL or an empty string leaves averaging disabled.
void gui_set_averages(const gchar *spec) {
    if (avg)
        avg_windows_free(avg);
    avg = avg_windows_parse(spec, REFRESH_INTERVAL_MS);
}

// Restrict which sensors get averaged to those whose label contains one of the
// given comma-separated substrings. NULL/empty averages every sensor.
void gui_set_average_filter(const gchar *spec) {
    str_filter_free(avg_filter);
    avg_filter = str_filter_parse(spec);
}

static guint window_width(void) {
    return 500 + avg_count() * 100;
}

static void init_sensors() {
    GtkTreeIter iter;
    GSList *sensor;
    GtkListStore *store;
    SensorSource *source;
    const SensorInit *data;
    guint i = 0, k;

    store = GTK_LIST_STORE(model);
    for (source = sensor_sources; source->drv; source++) {
        if (source->func_init()){
            source->sensors = source->func_get_sensors();
            if (source->sensors != NULL) {
                source->enabled = TRUE;

                sensor = source->sensors;
                while (sensor) {
                    data = (SensorInit*)sensor->data;
                    gtk_list_store_append(store, &iter);
                    gtk_list_store_set(store, &iter,
                                       COLUMN_NAME,  data->label,
                                       COLUMN_HINT,  data->hint,
                                       COLUMN_VALUE, " --- ",
                                       COLUMN_MIN,   " --- ",
                                       COLUMN_MAX,   " --- ",
                                       -1);
                    for (k = 0; k < avg_count(); k++)
                        gtk_list_store_set(store, &iter, COLUMN_AVG_BASE + k, " --- ", -1);
                    sensor = sensor->next;
                    i++;
                }
            }
        }
    }

    // Allocate per-row history once we know how many rows exist. A second pass
    // (same iteration order) decides, per row, whether it is averaged, and only
    // those rows get a ring buffer allocated.
    n_rows = i;
    if (avg_count() > 0 && n_rows > 0) {
        guint r = 0;
        series = g_new0(AvgSeries, n_rows);
        row_avg = g_new0(gboolean, n_rows);

        for (source = sensor_sources; source->drv; source++) {
            if (!source->enabled)
                continue;
            for (sensor = source->sensors; sensor; sensor = sensor->next) {
                data = (SensorInit *)sensor->data;
                row_avg[r] = str_filter_match(avg_filter, data->label);
                if (row_avg[r])
                    avg_series_init(&series[r], avg);
                r++;
            }
        }
    }
}

static GtkTreeModel* create_model (void) {
    GtkListStore *store;
    guint n_columns = COLUMN_AVG_BASE + avg_count();
    GType *types = g_new(GType, n_columns);
    guint c;

    for (c = 0; c < n_columns; c++)
        types[c] = G_TYPE_STRING;

    store = gtk_list_store_newv(n_columns, types);
    g_free(types);
    return GTK_TREE_MODEL (store);
}

static void set_list_column_value(float num, const gchar *printf_format, GtkTreeIter *iter, gint column){
    gchar *value;
    if (num != ERROR_VALUE)
        value = g_strdup_printf(printf_format, num);
    else
        value = g_strdup("    ? ? ?");
    gtk_list_store_set(GTK_LIST_STORE (model), iter, column, value, -1);
    g_free(value);
}

// Push a fresh reading into a row's history and refresh its average columns.
static void update_averages(guint row, float value, const gchar *printf_format, GtkTreeIter *iter) {
    guint k;

    if (avg_count() == 0 || row >= n_rows || !row_avg[row])
        return;

    avg_series_push(&series[row], avg, value);
    if (!series[row].valid)
        return;

    for (k = 0; k < avg->count; k++)
        set_list_column_value((float)series[row].avg[k], printf_format, iter,
                              COLUMN_AVG_BASE + k);
}

static gboolean update_data (gpointer data) {
    GtkTreeIter iter;
    GSList *node;
    SensorSource *source;
    const SensorInit *sensorData;
    guint row = 0;

    if (model == NULL)
        return G_SOURCE_REMOVE;

    if (!gtk_tree_model_get_iter_first (model, &iter))
        return G_SOURCE_REMOVE;

    for (source = sensor_sources; source->drv; source++) {
        if (!source->enabled)
            continue;

        source->func_update();
        if (source->sensors){
            node = source->sensors;

            while(node) {
                sensorData = (SensorInit*)node->data;
                set_list_column_value(*(sensorData->value), sensorData->printf_format, &iter, COLUMN_VALUE);
                set_list_column_value(*(sensorData->min), sensorData->printf_format, &iter, COLUMN_MIN);
                set_list_column_value(*(sensorData->max), sensorData->printf_format, &iter, COLUMN_MAX);
                update_averages(row, *(sensorData->value), sensorData->printf_format, &iter);

                row++;
                node = node->next;
                if (!gtk_tree_model_iter_next(model, &iter))
                    break;
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

static void append_text_column(GtkTreeView *treeview, const gchar *title, gint column) {
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *col;

    renderer = gtk_cell_renderer_text_new ();
    col = gtk_tree_view_column_new_with_attributes (title, renderer,
                                                    "text", column,
                                                    NULL);
    g_object_set(renderer, "family", "monotype", NULL);
    gtk_tree_view_append_column (treeview, col);
}

static void add_columns (GtkTreeView *treeview) {
    guint k;

    append_text_column(treeview, "Sensor", COLUMN_NAME);
    append_text_column(treeview, "Value", COLUMN_VALUE);
    append_text_column(treeview, "Min", COLUMN_MIN);
    append_text_column(treeview, "Max", COLUMN_MAX);

    for (k = 0; k < avg_count(); k++)
        append_text_column(treeview, avg->titles[k], COLUMN_AVG_BASE + k);
}

static void about_btn_clicked(GtkButton *button, gpointer user_data) {
    GtkWidget *dialog;
    const gchar *website = "https://github.com/ocerman/zenmonitor";
    const gchar *msg = "<b>Zen Monitor</b> %s\n"
                       "Monitoring software for AMD Zen-based CPUs\n"
                       "<a href=\"%s\">%s</a>\n\n"
                       "Created by: Ondrej Čerman";

    dialog = gtk_message_dialog_new_with_markup(GTK_WINDOW (window),
                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                    GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                    msg, VERSION, website, website);

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void clear_btn_clicked(GtkButton *button, gpointer user_data) {
    SensorSource *source;

    for (source = sensor_sources; source->drv; source++) {
        if (!source->enabled)
            continue;

        source->func_clear_minmax();
    }
}

static gboolean mid_search_eq_func(GtkTreeModel *model, gint column, const gchar *key, GtkTreeIter *iter) {
    gchar *iter_string = NULL, *lc_iter_string = NULL, *lc_key = NULL;
    gboolean result;

    gtk_tree_model_get(model, iter, column, &iter_string, -1);
    lc_iter_string = g_utf8_strdown(iter_string, -1);
    lc_key = g_utf8_strdown(key, -1);

    result = (g_strrstr(lc_iter_string, lc_key) == NULL);

    g_free(iter_string);
    g_free(lc_iter_string);
    g_free(lc_key);

    return result;
}

static void resize_to_treeview(GtkWindow* window, GtkTreeView* treeview) {
    gint uiHeight, cellHeight, vSeparator, rows;
    GdkRectangle r;

    GtkTreeViewColumn *col = gtk_tree_view_get_column(treeview, 0);
    if (!col)
        return;

    gtk_tree_view_column_cell_get_size(col, NULL, NULL, NULL, NULL, &cellHeight);
    gtk_widget_style_get(GTK_WIDGET(treeview), "vertical-separator", &vSeparator, NULL);
    rows = gtk_tree_model_iter_n_children(gtk_tree_view_get_model(treeview), NULL);

    gtk_tree_view_get_visible_rect(treeview, &r);
    uiHeight = defaultHeight - r.height;

    gtk_window_resize(window, window_width(), uiHeight + (vSeparator + cellHeight) * rows);
}

int start_gui (SensorSource *ss) {
    GtkWidget *about_btn;
    GtkWidget *clear_btn;
    GtkWidget *box;
    GtkWidget *header;
    GtkWidget *treeview;
    GtkWidget *sw;
    GtkWidget *vbox;
    GtkWidget *dialog;

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size(GTK_WINDOW(window), window_width(), defaultHeight);

    gchar *cpu_model_str = cpu_model();
    header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR (header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR (header), "Zen monitor");
    gtk_header_bar_set_has_subtitle(GTK_HEADER_BAR (header), TRUE);
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR (header), cpu_model_str);
    gtk_window_set_titlebar (GTK_WINDOW (window), header);
    g_free(cpu_model_str);

    box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class (gtk_widget_get_style_context (box), "linked");

    about_btn = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(about_btn), gtk_image_new_from_icon_name("dialog-information", GTK_ICON_SIZE_BUTTON));
    gtk_container_add(GTK_CONTAINER(box), about_btn);
    gtk_widget_set_tooltip_text(about_btn, "About Zen monitor");

    clear_btn = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(clear_btn), gtk_image_new_from_icon_name("edit-clear-all", GTK_ICON_SIZE_BUTTON));
    gtk_container_add(GTK_CONTAINER(box), clear_btn);
    gtk_widget_set_tooltip_text(clear_btn, "Clear Min/Max");

    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), box);
    g_signal_connect(about_btn, "clicked", G_CALLBACK(about_btn_clicked), NULL);
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(clear_btn_clicked), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER (window), vbox);

    sw = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_ETCHED_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX (vbox), sw, TRUE, TRUE, 0);

    model = create_model();
    treeview = gtk_tree_view_new_with_model(model);
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(treeview), COLUMN_HINT);

    gtk_container_add (GTK_CONTAINER(sw), treeview);
    add_columns(GTK_TREE_VIEW(treeview));
    gtk_widget_show_all(window);

    gtk_tree_view_set_search_column(GTK_TREE_VIEW(treeview), COLUMN_NAME);
    gtk_tree_view_set_search_equal_func(GTK_TREE_VIEW(treeview),
        (GtkTreeViewSearchEqualFunc)mid_search_eq_func, model, NULL);

    g_object_unref(model);

    if (check_zen()){
        sensor_sources = ss;
        init_sensors();

        resize_to_treeview(GTK_WINDOW(window), GTK_TREE_VIEW(treeview));
        timeout = g_timeout_add(REFRESH_INTERVAL_MS, update_data, NULL);
    }
    else{
        dialog = gtk_message_dialog_new(GTK_WINDOW (window),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                        "Zen CPU not detected!");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }

    gtk_main();
    return 0;
}
