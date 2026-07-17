# Zen monitor
Zen monitor is monitoring software for AMD Zen-based CPUs.

It can monitor these values:
 - CPU Temperature
 - CPU Core (SVI2) Voltage, Current and Power
 - SOC (SVI2) Voltage, Current and Power
 - Package and Core Power (RAPL)
 - Core Frequency (from OS)

![screenshot](screenshot.png)

## Dependencies
 - [zenpower driver](https://github.com/ocerman/zenpower/) - For monitoring CPU temperature and SVI2 sensors
 - MSR driver - For monitoring Package/Core Power (RAPL)

Follow [zenpower README.md](https://github.com/ocerman/zenpower/blob/master/README.md) to install and activate zenpower module.
Enter `sudo modprobe msr` to enable MSR driver.

## Building 
Make sure that GTK3 dev package and common build tools are installed.
```
make
```

## Launching
You can launch app by `sudo ./zenmonitor`, or you can install it to your system and then launch it from your OS menu.

Note: Because superuser privileges are usually needed to access data from MSR driver, you need to launch zenmonitor as root for monitoring CPU power usage (RAPL).
Alternatively, you can set capabilities to zenmonitor executable: `sudo setcap cap_sys_rawio,cap_dac_read_search+ep ./zenmonitor`

## Command line arguments

``--coreid`` - Display core_id instead of core index

``--interval MS`` - Initial refresh interval in milliseconds (50-60000, default 1000). The interval is also adjustable while running via the "Update interval" button in the header bar; changing it resets the rolling averages.

``--average WINDOWS`` - Show additional rolling-average columns for the given comma-separated time windows, e.g. ``--average 30s,1m,5m`` (suffixes: ``s`` seconds, ``m`` minutes, ``h`` hours; a bare number is seconds). Omit to show no average columns.

``--average-only SUBSTRINGS`` - Only average sensors whose label contains one of these comma-separated substrings (case-insensitive), e.g. ``--average-only power,temp``. Non-matching rows still show Value/Min/Max but leave the average cells blank. Omit to average every sensor.

## Command line interface (zenmonitor-cli)
A headless build is available for terminals and panels:
```
make build-cli
sudo make install-cli
```
It reuses the same sensor backends and supports ``--delay SECONDS`` (poll
interval), ``--coreid``, ``--refresh-in-place`` (redraw in place), ``--output-once``,
and ``--file FILE`` (stream readings to a CSV file, one row per refresh). The CSV
is appended and flushed as it goes, so memory use stays constant, ``tail -f``
works on it, and the log survives up to the last row if the machine crashes -
which makes it suitable for long high-frequency captures.

``--sensors SUBSTRINGS`` limits output to sensors whose label contains one of the
given comma-separated substrings (case-insensitive), e.g.
``--sensors "temperature,package power"``. Besides trimming the output and the
per-sensor average buffers, if a whole backend has no matching sensors its
per-tick read is skipped entirely (handy to avoid the MSR reads when you only
want temperatures).

### Rolling averages in a panel (daemon mode)
A rolling average needs a process that has been sampling for the whole window,
so short-lived panel commands can't compute one on their own. Run zenmonitor-cli
as a small resident daemon instead:
```
zenmonitor-cli --daemon --delay 1 --average 1m,5m
```
It samples every ``--delay`` seconds and rewrites a snapshot file (default
``$XDG_RUNTIME_DIR/zenmonitor.snapshot``, override with ``--snapshot FILE``)
containing each sensor's current value and the configured rolling averages.
The window→sample conversion follows ``--delay``, so ``5m`` is five minutes at
any poll rate.

``data/zenmonitor-cli.service`` is an example systemd *user* unit that keeps the
daemon running. ``data/zenmonitor-genmon.sh`` reads the snapshot for the
[xfce4-genmon-plugin](https://docs.xfce.org/panel-plugins/xfce4-genmon-plugin);
point a genmon item at, for example:
```
zenmonitor-genmon.sh "CPU Temperature (tCtl)" "Avg 1m"
```

Note: temperature/SVI2 sensors (via the zenpower driver) work as a normal user,
but RAPL package/core power needs MSR privileges. To include power in the daemon
snapshot, grant capabilities to the binary:
```
sudo setcap cap_sys_rawio,cap_dac_read_search+ep /usr/local/bin/zenmonitor-cli
```

## Installing
By default, Zenmonitor will be installed to /usr/local.
```
sudo make install
```

To add menu item for launching zenpower as root (Polkit is required):
```
sudo make install-polkit
```

## Uninstalling
```
sudo make uninstall
```

## Setup on ubuntu
First follow [installation instructions on zenpower](https://github.com/ocerman/zenpower/blob/master/README.md#installation-commands-for-ubuntu)
Then:
```
sudo modprobe msr
sudo bash -c 'echo "msr" > /etc/modules-load.d/msr.conf'
sudo apt install build-essential libgtk-3-dev git
cd ~
git clone https://github.com/HonsW/zenmonitor
cd zenmonitor
make
sudo make install
sudo make install-polkit
```
