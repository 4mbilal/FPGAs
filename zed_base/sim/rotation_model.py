#!/usr/bin/env python3
"""rotation_model.py - discrete-event model of the buffer rotation in sw/common/jig.c.

Hardware model: each frame buffer DMA latches its address register at frame start; with
auto-restart a frame starts the moment the previous one is done, and the 1-bit done flag
is set then (several dones while the CPU is busy collapse into one flag).
Software model: the polling loop of jig.c, including slow SW-path kernels that block
polling, and an injected race between polling the display flag and writing a new address.
Counts camera writes into a buffer that is being displayed or processed (tearing).

  python3 sim/rotation_model.py
"""
import random


def run(sw_path, seed, fixed=True, T=8_000_000, cam_period=33_333, race=True):
    rnd = random.Random(seed)
    NB = 6
    s = dict(cw=1, cp=2, dr=0, ds=0, dp=-1, dpage=0, raw=-1, hold=-1)
    reg = dict(wr=2, rd=0)
    lat = dict(wr=1, rd=0)
    flag = dict(wr=0, rd=0)
    st = dict(nc=cam_period + rnd.randint(0, 9000), nd=16_667, t=0, viol=0)

    def busy(i):
        return i in (s['cw'], s['cp'], s['dr'], s['ds'], s['dp'], s['raw'], s['hold'])

    def pick():
        for i in range(NB):
            if not busy(i):
                return i
        raise RuntimeError("no free buffer")

    def adv(to):
        while min(st['nc'], st['nd']) <= to:
            if st['nc'] <= st['nd']:
                flag['wr'] = 1; lat['wr'] = reg['wr']; st['nc'] += cam_period + rnd.randint(-200, 200)
            else:
                flag['rd'] = 1; lat['rd'] = reg['rd']; st['nd'] += 16_667
            if lat['wr'] == lat['rd'] or lat['wr'] == s['hold']:
                st['viol'] += 1

    def rd_event():
        s['ds'] = s['dr']
        if fixed:
            if s['dp'] >= 0:
                s['dpage'] += 1
                if s['dpage'] >= 2:
                    s['dp'] = -1
        else:
            s['dp'] = -1

    def display(i):
        if fixed and flag['rd']:
            flag['rd'] = 0; rd_event()
        if race and rnd.random() < 0.3:
            st['t'] += rnd.randint(1, 40); adv(st['t'])
        s['dp'] = s['dr']; s['dpage'] = 0; s['dr'] = i; reg['rd'] = i

    while st['t'] < T:
        st['t'] += rnd.randint(1, 3); adv(st['t'])
        if flag['rd']:
            flag['rd'] = 0; rd_event()
        st['t'] += rnd.randint(1, 3); adv(st['t'])
        if flag['wr']:
            flag['wr'] = 0
            f = s['cw']; s['cw'] = s['cp']
            if not sw_path:
                display(f)
            else:
                s['raw'] = f
            s['cp'] = pick(); reg['wr'] = s['cp']
        if sw_path and s['raw'] >= 0:
            s['hold'] = s['raw']; s['raw'] = -1
            end = st['t'] + rnd.choice([0, 3000, 20000, 60000, 150000])
            while st['t'] < end:
                st['t'] = min(end, st['t'] + 100); adv(st['t'])
            display(s['hold']); s['hold'] = -1
    return st['viol']


if __name__ == "__main__":
    for fixed in (False, True):
        for sw in (0, 1):
            for per, name in ((66_667, "15 fps"), (33_333, "30 fps"), (16_667, "60 fps")):
                v = sum(run(sw, k, fixed, cam_period=per) for k in range(8))
                print(f"{'current ' if fixed else 'v4 logic'} {'SW' if sw else 'HW'} path, camera {name:6s}: collisions {v}")
