#include <glib.h>
#include <cpuid.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "zenmonitor.h"
#include "msr.h"
#include "sysfs.h"

#define MSR_PWR_PRINTF_FORMAT " %8.3f W"
#define MSR_FID_PRINTF_FORMAT " %8.3f GHz"

// AMD PPR  = https://www.amd.com/system/files/TechDocs/54945_PPR_Family_17h_Models_00h-0Fh.pdf
// AMD OSRR = https://developer.amd.com/wp-content/resources/56255_3_03.PDF

static guint cores = 0;
static gdouble energy_unit = 0;
static struct cpudev *cpu_dev_ids;

// Monotonic timestamp (microseconds) of the previous sample. RAPL energy is a
// running counter, so power is the energy delta divided by the real elapsed
// time between samples. Measuring over the natural refresh interval avoids a
// blocking sleep that would otherwise freeze the GTK main loop each tick.
static gint64 last_sample_time = 0;

static gint *msr_files = NULL;

static gulong package_eng_b = 0;
static gulong package_eng_a = 0;
static gulong *core_eng_b = NULL;
static gulong *core_eng_a = NULL;

static gfloat package_power;
static gfloat package_power_min;
static gfloat package_power_max;
static gfloat *core_power;
static gfloat *core_fid;
static gfloat *core_power_min;
static gfloat *core_power_max;
static gfloat *core_fid_min;
static gfloat *core_fid_max;


static gint open_msr(gshort devid) {
    gchar msr_path[32];
    snprintf(msr_path, sizeof msr_path, "/dev/cpu/%d/msr", devid);
    return open(msr_path, O_RDONLY);
}

static gboolean read_msr(gint file, guint index, gulong *data) {
    if (file < 0)
        return FALSE;

    return pread(file, data, sizeof *data, index) == sizeof *data;
}

gdouble get_energy_unit() {
    gulong data;
    // AMD OSRR: page 139 - MSRC001_0299
    if (!read_msr(msr_files[0], 0xC0010299, &data))
        return 0.0;

    return pow(1.0/2.0, (double)((data >> 8) & 0x1F));
}

gulong get_package_energy() {
    gulong data;
    // AMD OSRR: page 139 - MSRC001_029B
    if (!read_msr(msr_files[0], 0xC001029B, &data))
        return 0;

    return data;
}

gulong get_core_energy(gint core) {
    gulong data;
    // AMD OSRR: page 139 - MSRC001_029A
    if (!read_msr(msr_files[core], 0xC001029A, &data))
        return 0;

    return data;
}

gdouble get_core_fid(gint core) {
    gdouble ratio;
    gulong data, fdid;

    // By reverse-engineering Ryzen Master, we know that
    //  this undocumented MSR is responsible for returning
    //  the FID and FDID for the core used for calculating the
    //  effective frequency.
    //
    // The FID is returned in bits [8:0]
    // The FDID is returned in bits [14:8]
    if (!read_msr(msr_files[core], 0xC0010293, &data))
        return 0;

    fdid = (data >> 8) & 0x3F;
    if (fdid == 0)
        return 0;

    ratio = (gdouble)(data & 0xff) / (gdouble)fdid;

    // The effective ratio is based on increments of 200 MHz.
    return ratio * 200.0 / 1000.0;
}

gboolean msr_init(void) {
    guint i;

    if (!check_zen())
        return FALSE;

    cores = get_core_count();
    if (cores == 0)
        return FALSE;

    cpu_dev_ids = get_cpu_dev_ids();
    msr_files = g_malloc(cores * sizeof (gint));
    for (i = 0; i < cores; i++) {
        msr_files[i] = open_msr(cpu_dev_ids[i].cpuid);
    }

    energy_unit = get_energy_unit();
    if (energy_unit == 0)
        return FALSE;

    core_eng_b = g_malloc(cores * sizeof (gulong));
    core_eng_a = g_malloc(cores * sizeof (gulong));
    core_power = g_malloc(cores * sizeof (gfloat));
    core_fid = g_malloc(cores * sizeof (gfloat));
    core_power_min = g_malloc(cores * sizeof (gfloat));
    core_power_max = g_malloc(cores * sizeof (gfloat));
    core_fid_min = g_malloc(cores * sizeof (gfloat));
    core_fid_max = g_malloc(cores * sizeof (gfloat));

    // Establish the energy/time baseline. Power stays at 0 until the first
    // timer-driven msr_update() computes it over the elapsed interval.
    last_sample_time = g_get_monotonic_time();
    package_eng_b = get_package_energy();
    package_power = 0;
    for (i = 0; i < cores; i++) {
        core_eng_b[i] = get_core_energy(i);
        core_power[i] = 0;
        core_fid[i] = get_core_fid(i);
    }

    memcpy(core_power_min, core_power, cores * sizeof (gfloat));
    memcpy(core_power_max, core_power, cores * sizeof (gfloat));
    memcpy(core_fid_min, core_fid, cores * sizeof (gfloat));
    memcpy(core_fid_max, core_fid, cores * sizeof (gfloat));
    package_power_min = package_power;
    package_power_max = package_power;

    return TRUE;
}

void msr_update(void) {
    guint i;
    gint64 now;
    gdouble elapsed;

    // Read the current energy counters and compute power over the time that
    // has actually elapsed since the previous sample. No blocking sleep, so
    // the GTK main loop stays responsive between refreshes.
    now = g_get_monotonic_time();
    elapsed = (now - last_sample_time) / 1000000.0;

    package_eng_a = get_package_energy();
    for (i = 0; i < cores; i++) {
        core_eng_a[i] = get_core_energy(i);
    }

    if (elapsed > 0) {
        if (package_eng_a >= package_eng_b) {
            package_power = (package_eng_a - package_eng_b) * energy_unit / elapsed;

            if (package_power < package_power_min)
                package_power_min = package_power;
            if (package_power > package_power_max)
                package_power_max = package_power;
        }

        for (i = 0; i < cores; i++) {
            if (core_eng_a[i] >= core_eng_b[i]) {
                core_power[i] = (core_eng_a[i] - core_eng_b[i]) * energy_unit / elapsed;

                if (core_power[i] < core_power_min[i])
                    core_power_min[i] = core_power[i];
                if (core_power[i] > core_power_max[i])
                    core_power_max[i] = core_power[i];
            }
        }
    }

    for (i = 0; i < cores; i++) {
        core_fid[i] = get_core_fid(i);

        if (core_fid[i] < core_fid_min[i])
            core_fid_min[i] = core_fid[i];
        if (core_fid[i] > core_fid_max[i])
            core_fid_max[i] = core_fid[i];
    }

    // Current counters become the baseline for the next interval.
    package_eng_b = package_eng_a;
    for (i = 0; i < cores; i++) {
        core_eng_b[i] = core_eng_a[i];
    }
    last_sample_time = now;
}

void msr_clear_minmax(void) {
    guint i;

    package_power_min = package_power;
    package_power_max = package_power;
    for (i = 0; i < cores; i++) {
        core_power_min[i] = core_power[i];
        core_power_max[i] = core_power[i];
        core_fid_min[i] = core_fid[i];
        core_fid_max[i] = core_fid[i];
    }
}

GSList* msr_get_sensors(void) {
    GSList *list = NULL;
    SensorInit *data;
    guint i;

    data = sensor_init_new();
    data->label = g_strdup("Package Power");
    data->hint = g_strdup("Package Power reported by RAPL\nSource: cpu0 MSR");
    data->value = &package_power;
    data->min = &package_power_min;
    data->max = &package_power_max;
    data->printf_format = MSR_PWR_PRINTF_FORMAT;
    list = g_slist_append(list, data);

    for (i = 0; i < cores; i++) {
        data = sensor_init_new();
        data->label = g_strdup_printf("Core %d Effective Frequency", display_coreid ? cpu_dev_ids[i].coreid: i);
        data->hint = g_strdup_printf("Source: cpu%d MSR", cpu_dev_ids[i].cpuid);
        data->value = &(core_fid[i]);
        data->min = &(core_fid_min[i]);
        data->max = &(core_fid_max[i]);
        data->printf_format = MSR_FID_PRINTF_FORMAT;
        list = g_slist_append(list, data);
    }

    for (i = 0; i < cores; i++) {
        data = sensor_init_new();
        data->label = g_strdup_printf("Core %d Power", display_coreid ? cpu_dev_ids[i].coreid: i);
        data->hint = g_strdup_printf("Core Power reported by RAPL\nSource: cpu%d MSR", cpu_dev_ids[i].cpuid);
        data->value = &(core_power[i]);
        data->min = &(core_power_min[i]);
        data->max = &(core_power_max[i]);
        data->printf_format = MSR_PWR_PRINTF_FORMAT;
        list = g_slist_append(list, data);
    }

    return list;
}
