## Configure PTP

### Local PTP (reliable setup)

#### 0. Install linuxptp

```bash
sudo apt install linuxptp
```

#### 1. Configure `/etc/ptp4l.conf`

Create `/etc/ptp4l.conf` if needed:

```ini
[global]
verbose 1
boundary_clock_jbod 1
logSyncInterval -4
tx_timestamp_timeout 50
```

`boundary_clock_jbod 1` requires `phc2sys` to keep PHCs and system time aligned.

#### 2. Disable competing system time sync while using PTP

```bash
sudo timedatectl set-ntp false
```

If you later stop using PTP, you can re-enable with:

```bash
sudo timedatectl set-ntp true
```

#### 3. Start `ptp4l` and `phc2sys` (systemd units via the helper script)

On pancake0, `ptp4l` and `phc2sys` run as the systemd units `ptp4l.service`
and `phc2sys.service`, installed and enabled for boot by pancakebatter
(`sudo ~/pancakebatter/install_ptp_units.sh`, see pancakebatter
`docs/ptp_services.md`). The units carry the camera interfaces, the CPU
affinity off the isolated cores, and an `ExecStartPost` setter that declares
the 37 s TAI-UTC offset valid. Never start the daemons by hand: a hand-started
`ptp4l` keeps `currentUtcOffsetValid 0` and every recording then classifies
the cameras `ieee1588_ptp_timescale_untraceable` (this happened from
2026-10-17 to 2026-10-27, when older versions of this script ran
`sudo -b ptp4l` and `pkill`).

`scripts/ptp_stack.sh` drives the units:

```bash
./scripts/ptp_stack.sh start     # systemctl start ptp4l.service phc2sys.service
./scripts/ptp_stack.sh status    # processes, TIME_STATUS_NP, evidence, unit state
./scripts/ptp_stack.sh logs      # journalctl -u ptp4l.service -u phc2sys.service -f
./scripts/ptp_stack.sh stop      # systemctl stop (and kills hand-started daemons)
./scripts/ptp_stack.sh restart   # replaces hand-started daemons with the units
```

`status` warns when a daemon runs outside its unit or when the grandmaster
does not declare the offset valid. Stopping or restarting `ptp4l` drops the
cameras' PTP lock: only with no `orange` or `orange_client` running.

Headless `ptp_gate` experiment runs may auto-start this host stack when they
detect that `ptp4l` / `phc2sys` are not ready, and leave it running on exit.

If Orange is running with sufficient privileges (for example via `sudo`), the
main UI now also exposes a `Host PTP Stack` section with:

- `Start PTP stack`
- `Refresh PTP status`

Orange never stops or restarts the stack (since 2026-11-04). A `Stop PTP stack`
button next to `Refresh PTP status` stopped it by accident that day, and the
PTP-gated cameras then delivered no frames. Stop or restart it from a terminal
with no `orange` or `orange_client` running.

The UI calls `scripts/ptp_stack.sh` directly and shows captured stdout/stderr,
plus a compact status summary for `ptp4l`, `phc2sys`, and socket presence.

GUI validation note: manual GUI use can start the host stack from the
`Host PTP Stack` panel. For automated GUI validation, the privileged
`orange-gui-validation` wrapper supports `--ptp-stack-mode off|require|auto`;
`scripts/run_gui_aq_off_validation.sh` defaults to `auto` for
`ORANGE_GUI_AUTORUN=1` with `ptp_gate` configs. That path checks
`ptp4l`/`phc2sys` and `/var/run/ptp4l`, starts the stack if needed, and
rechecks before launching Orange.

If you are running the GUI manually or with `ORANGE_GUI_PTP_STACK_MODE=off`,
check the stack first:

```bash
sudo -n ./scripts/ptp_stack.sh status
```

If the status shows missing `ptp4l`, `phc2sys`, or `/var/run/ptp4l`, either
click `Start PTP stack` in the GUI panel or start the stack and recheck from
the shell:

```bash
sudo -n ./scripts/ptp_stack.sh start
sudo -n ./scripts/ptp_stack.sh status
```

Optional shell alias:

```bash
alias ptp-stack='~/orange-jeremy/scripts/ptp_stack.sh'
```

Then use:

```bash
ptp-stack start
ptp-stack status
ptp-stack logs
ptp-stack stop
```

#### 4. Manual commands (without helper script)

Diagnostics only, never on pancake0 for recordings: these commands skip the
unit's offset setter, so the cameras classify
`ieee1588_ptp_timescale_untraceable`. Stop them and run
`./scripts/ptp_stack.sh start` afterwards.

Start `ptp4l` with all camera NIC ports:

```bash
sudo ptp4l -i mlnx1_p1_25g -i mlnx1_p2_25g -i mlnx2_p3_25g -i mlnx2_p4_25g -f /etc/ptp4l.conf -m
```

In another terminal, start `phc2sys`:

```bash
sudo phc2sys -a -rr -z /var/run/ptp4l -m
```

Important: on some linuxptp versions, `-w` is not compatible with `-a`. If you see:
`autoconfiguration cannot be mixed with manual config options`, remove `-w`.

#### 5. Verify

```bash
pgrep -af "ptp4l|phc2sys"
sudo pmc -u -b 0 -s /var/run/ptp4l "GET TIME_STATUS_NP" "GET CURRENT_DATA_SET" "GET PORT_DATA_SET"
```

Healthy indicators:
- `ptp4l` ports stay in expected role (`MASTER` or `SLAVE` based on topology)
- `phc2sys` offsets are small and stable
- no other service is trying to discipline system time at the same time

#### 6. Compare recorded camera timestamps

Use:

```bash
./scripts/compare_camera_timestamps.py <recording_folder>
```

Example:

```bash
./scripts/compare_camera_timestamps.py /home/jeremy/orange_data/exp/unsorted/<session>/<recording_id>
```

This compares `Cam*_meta.csv` files in the folder and prints per-camera skew
relative to a reference camera.

Useful options:

```bash
# Use system clock timestamps instead of camera timestamps
./scripts/compare_camera_timestamps.py <recording_folder> --timestamp-field timestamp_sys

# Pick a specific reference camera
./scripts/compare_camera_timestamps.py <recording_folder> --reference 2010096

# Save summary CSV
./scripts/compare_camera_timestamps.py <recording_folder> --summary-out /tmp/ptp_skew_summary.csv
```

Notes:
- Positive skew means a camera timestamp is later than the reference.
- If a folder only has one camera metadata file, skew comparison is not possible.


### Network PTP using switch 
If you are using network switch, please enable PTP on all the ports connected to either the camera or host NICs. Here is an example of setting it with Arista and Mellanox (Onyx) switch.

Once logged in: 

```
en
```

```
conf t
```

```
show ptp 
```

if ptp is disabled: 

```
ptp mode boundary 
```

config each port 

```
show interface
``` 


Example to enable ptp on port 5
```
interface Ethernet 5
ptp enable 

```

To config multiple ports:

```
interface Ethernet49/1-Ethernet60/1  
ptp enable 
```

```
interface Ethernet1-Ethernet48  
ptp enable 
```

```
show int sta
```

if there is errdisabled, try to restart the port

```
interface Ethernet ##
shutdown 
no shutdown 
```

To disable the spanning tree 

```
spanning-tree bpduguard disable
```


## Mellanox

[Document](https://safe.nrao.edu/wiki/pub/Beamformer/DDLTestingCommModeOne/MLNX-OS_SW_Eth_3.2.0506_Command_Reference_Guide.pdf)

Userful command: 

```
show interface ethernet 1/11
```

```
enable 
configure terminal 
interface ethernet 1/11
shutdown
mtu 9216
speed 25000
fec rs-fec
no shutdown
```

To config multiple ports: 
```
interface ethernet 1/1-1/16 shutdown  
interface ethernet 1/1-1/16 mtu 9216  
interface ethernet 1/1-1/16 fec rs-fec  
interface ethernet 1/1-1/16 speed 25G  
interface ethernet 1/1-1/16 no shutdown

```
