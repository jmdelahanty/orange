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

#### 3. Start `ptp4l` and `phc2sys` (recommended helper script)

Use the helper at `scripts/ptp_stack.sh`:

```bash
./scripts/ptp_stack.sh start
./scripts/ptp_stack.sh status
./scripts/ptp_stack.sh logs
```

Stop:

```bash
./scripts/ptp_stack.sh stop
```

Headless `ptp_gate` experiment runs may auto-start this host stack when they
detect that `ptp4l` / `phc2sys` are not ready. When a run repairs the host PTP
stack this way, it leaves the stack running on exit so follow-up PTP validation
does not silently lose clock sync. After the final PTP run, stop it explicitly:

```bash
./scripts/ptp_stack.sh stop
```

You can confirm it is stopped when:

```bash
pgrep -af "ptp4l|phc2sys"   # no output
ls -l /var/run/ptp4l         # no such file
```

If Orange is running with sufficient privileges (for example via `sudo`), the
main UI now also exposes a `Host PTP Stack` section with:

- `Start PTP stack`
- `Refresh PTP status`
- `Stop PTP stack`
- `Restart PTP stack`

The UI calls `scripts/ptp_stack.sh` directly and shows captured stdout/stderr,
plus a compact status summary for `ptp4l`, `phc2sys`, and socket presence.

GUI validation note: the GUI `ptp_gate` streaming path currently does not
auto-repair a stopped host PTP stack the way headless validation can. If
`ptp4l`/`phc2sys` are stopped, starting a GUI PTP stream may leave
`Streaming FPS` and `YOLO FPS` at `0` while startup waits in PTP offset/gate
setup before acquisition begins. Before GUI PTP validation, run:

```bash
sudo -n ./scripts/ptp_stack.sh status
```

If the status shows missing `ptp4l`, `phc2sys`, or `/var/run/ptp4l`, start the
stack and recheck:

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
