#!/usr/bin/env python3
"""Report the record-start transition from recording_startup_audit.jsonl.

Answers: which preparation milestones happened after recording was enabled,
which first-use operations were slow, and how they sit relative to camera
frame-id gaps. Usage: recording_startup_audit_report.py <recording folder>
"""
import json, os, re, sys
from collections import defaultdict

folder = sys.argv[1] if len(sys.argv) > 1 else '.'
path = os.path.join(folder, 'recording_startup_audit.jsonl')
if not os.path.exists(path):
    sys.exit(f'no audit file at {path}')
rows = [json.loads(l) for l in open(path) if l.strip()]
session = rows[0] if rows and rows[0].get('record') == 'session' else {}
events = [r for r in rows if r.get('record') != 'session']
enabled = session.get('recording_enabled_steady_ns', 0)
print(f"session {session.get('session_id')} records={session.get('records')} overflow={session.get('overflow')} window_s={session.get('window_s')}")
if not enabled:
    print('recording_enabled never marked; times are relative to session_begin')

def rel(r):
    return r['t_rel_enabled_ms']

def kv(detail):
    return dict(re.findall(r'(\w+)=([^ ]+)', detail))

milestones = [r for r in events if r['milestone'] not in ('ingress_frame', 'owner_push_decision')]
print('\n== milestones (ms relative to recording_enabled; negative = before) ==')
for r in sorted(milestones, key=rel):
    flag = '  <-- after recording enabled' if enabled and rel(r) > 0 and r['milestone'] not in (
        'recording_enabled', 'first_frame_submitted', 'first_frame_acked', 'first_owner_push_done',
        'camera_frame_gap', 'recording_stop_requested', 'owner_push_stream_create_start',
        'owner_push_stream_create_done') else ''
    if r['milestone'] in ('owner_push_stream_create_start', 'owner_push_stream_create_done') and rel(r) > 0:
        flag = '  <-- first-use work after recording enabled'
    if r['milestone'] == 'camera_frame_gap':
        flag = '  <== GAP'
    print(f"{rel(r):10.1f}  {r['camera'] or '-':8s} {r['milestone']:32s} frame={r['frame_id']:<6d} {r['detail'][:110]}{flag}")

print('\n== first-use durations ==')
by_cam = defaultdict(dict)
for r in milestones:
    by_cam[r['camera']][r['milestone']] = r
for cam, ms in sorted(by_cam.items()):
    if not cam:
        continue
    parts = []
    if 'owner_push_stream_create_start' in ms and 'owner_push_stream_create_done' in ms:
        parts.append(f"push_stream_create={rel(ms['owner_push_stream_create_done']) - rel(ms['owner_push_stream_create_start']):.2f} ms")
    if 'first_owner_push_done' in ms:
        parts.append(f"first_push={kv(ms['first_owner_push_done']['detail']).get('push_ms', '?')} ms at {rel(ms['first_owner_push_done']):.0f} ms")
    slots = [r for r in events if r['camera'] == cam and r['milestone'] == 'staging_slot_imported']
    if slots:
        opens = [float(kv(s['detail']).get('ipc_open_ms', 0)) for s in slots]
        parts.append(f"slots={len(slots)} ipc_open max={max(opens):.2f} ms last_import_at={max(rel(s) for s in slots):.0f} ms")
    if 'first_frame_submitted' in ms and 'first_frame_acked' in ms:
        parts.append(f"first_ack_after_submit={rel(ms['first_frame_acked']) - rel(ms['first_frame_submitted']):.1f} ms")
    print(f"  {cam}: " + '; '.join(parts))

print('\n== per-frame window: owner-push decisions and ingress waits ==')
dec = defaultdict(lambda: defaultdict(int))
waits = defaultdict(list)
for r in events:
    if r['milestone'] == 'owner_push_decision':
        dec[r['camera']][kv(r['detail']).get('kind', '?')] += 1
    elif r['milestone'] == 'ingress_frame':
        d = kv(r['detail'])
        waits[r['camera']].append((float(d.get('source_ready_wait_ms', 0)), float(d.get('detach_ms', 0)), int(d.get('queue_in', 0)), r['frame_id'], rel(r)))
for cam in sorted(set(list(dec) + list(waits))):
    w = waits.get(cam, [])
    if w:
        sw = sorted(x[0] for x in w); dt = sorted(x[1] for x in w); q = max(x[2] for x in w)
        p = lambda a, f: a[min(len(a) - 1, int(f * len(a)))]
        slow = [f"f{x[3]}@{x[4]:.0f}ms wait={x[0]:.1f} detach={x[1]:.1f}" for x in w if x[0] > 15 or x[1] > 15][:6]
        print(f"  {cam}: frames={len(w)} source_ready_wait p50/p99/max={p(sw,.5):.2f}/{p(sw,.99):.2f}/{sw[-1]:.2f} ms detach p50/p99/max={p(dt,.5):.2f}/{p(dt,.99):.2f}/{dt[-1]:.2f} ms queue_in max={q} pushes={dict(dec.get(cam, {}))}" + (f" slow: {slow}" if slow else ''))

gaps = [r for r in events if r['milestone'] == 'camera_frame_gap']
print(f"\n== gaps: {len(gaps)} ==")
for g in sorted(gaps, key=rel):
    near = [m for m in milestones if m['milestone'] != 'camera_frame_gap' and abs(rel(m) - rel(g)) <= 50]
    print(f"  {rel(g):9.1f} ms {g['camera']} {g['detail'][:90]}  within 50 ms: {[ (m['camera'] or '-') + ':' + m['milestone'] for m in near ][:6]}")
