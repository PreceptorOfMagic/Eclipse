# The three stream modes, and what each one IS

Eclipse streams in exactly **three** states. They are not one configuration with tweaks — each
owns its complete set of switches, and every launch writes ALL of them, the ones it wants and
the ones it must not have. That rule exists because `/tmp` on the TV is wiped by standby, so
"it was set last time" is not a state, and a sentinel left over from another mode has more
than once made a broken run look like a working one.

| | mode 1 | mode 2 | mode 3 |
|---|---|---|---|
| | **Standard** | **Co-op vertical** | **Co-op horizontal** |
| geometry | full 4K, one host | two tile **columns** | two tile **rows** |
| pane | 3840x2160 | 1920x2160 each | 3840x1088 each |
| selector | no `coop` launch flag | `/tmp/coop-vmirror` | `/tmp/coop-hh-tiles` |
| settings | **existing Eclipse settings, untouched** | co-op tab | co-op tab |

**Mode 1 takes the ordinary Eclipse settings and nothing here applies to it.** A launch without
`"coop":true` logs `staying solo`; the co-op sentinels are config only and cannot activate it.

**Choosing co-op asks three things** — orientation, which PC is primary vs secondary, and which
controller goes to each. Everything else lives in the co-op settings tab. Eclipse asks them in
the co-op dialog when a session starts.

## Mode 3 — horizontal. CONFIRMED WORKING 2026-08-05, user-verified on the panel

Two real panes, PC1 primary + PC2 peer, live gameplay on both, 20+ minutes with **no decay**:
`skipconceal=6326` with the peer's tile intact, composite **90fps**, `repeated=0 peergap=7
stale=0` across 77k pictures. This is the reference configuration — reproduce it exactly before
concluding anything has regressed.

    COOP_ORIENTATION=horizontal COOP_PRIMARY=pc1 ./scripts/coop/run_coop.sh --no-motion

| switch | value | why |
|---|---|---|
| `/tmp/coop-hh-tiles` | **set** | tile rows. This IS horizontal now, not an experiment on top of it |
| `/tmp/coop-vmirror` | cleared | vertical's selector; leaving it set runs the wrong composition |
| `/tmp/coop-hmirror` | cleared | self-mirror composes the primary into BOTH tiles and ignores the peer — a one-host run that reads as two-host |
| `/tmp/coop-fps` | **absent** | no shared cap: each side free-runs 0..165fps |
| `/tmp/coop-slices` | `1` | one slice per host; `2` is the retired full-height design |
| `/tmp/coop-nocatchup` | cleared | catch-up ON — composite rate is `max(primary, peer)` |
| `/tmp/coop-noskipconceal` | cleared | skip-conceal ON — holds a tile when a host has no new frame |
| `/tmp/coop-playfile` | cleared | replays a recording while looking completely live |
| host encoders | `nvenc_constrained=1` both | without it every composed picture carries a 64-row corrupt band at the seam |
| host floors | low and **equal**, 30/30 | unequal floors manufacture peer oversupply; a high floor pads with DUPLICATE frames = latency |
| queue depth | 12 | shallower than `COOP_VT_CATCHUP_START` (4) and catch-up can never trigger |

**Conceal and catch-up are the load-bearing pair.** They are what let two free-running encoders
be spliced at all: catch-up covers the peer running ahead, skip-conceal covers it running behind
or sitting still. Both were dead in tiles mode until 2026-08-05 — they built slices over the
composite's parameters and `slice_header_end_bit_x()` refuses tiled parameters outright, so they
returned 0 every time. They now build over the pane's parameters and re-home through
`coop_slice_to_tile()`.

**Restoring the OLD forced-1:1 behaviour for an A/B:** set `COOP_FPS=<n>`, which writes
`/tmp/coop-fps` and caps both sessions (peer at `n-5`). `COOP_TILES=0` reaches the legacy
stacked-halves path. Neither is the working configuration.

## Mode 2 — vertical. CONFIRMED WORKING 2026-08-05, user-verified on the panel

Tile columns. Ran clean through the free-running change, the conceal/catch-up tile port and the
POC-hole fix: `evicted=0 broken=0 refused=0 pocback=0`, `recv == peer + catchup` (every received
pane delivered), 15 minutes with no decay signature and seam saturation 0.4–10.2 (magenta decay
reads 181).

    COOP_ORIENTATION=vertical COOP_PRIMARY=pc1 ./scripts/coop/run_coop.sh --no-motion

| switch | value | why |
|---|---|---|
| `/tmp/coop-vmirror` | **set** | tile columns — selects this mode |
| `/tmp/coop-hh-tiles` | cleared | horizontal's selector |
| `/tmp/coop-hmirror` | cleared | self-mirror: a one-host run that reads as two-host |
| `/tmp/coop-fps` | **absent** | no shared cap; each side free-runs |
| `/tmp/coop-slices` | `1` | one slice per host |
| `/tmp/coop-nocatchup` | cleared | catch-up ON |
| `/tmp/coop-noskipconceal` | cleared | skip-conceal ON |
| `/tmp/coop-playfile` | cleared | replays a recording while looking live |
| host encoders | `nvenc_constrained=1` both | seam band |
| host floors | **30/30, low and equal** | see below — changed 2026-08-05 |

**THE FLOORS WERE 110/165 AND ARE NOW 30/30, JUDGED BY FEEL.** The derived pair was measured in
the CAPPED era, when both hosts were forced to one rate to pair 1:1. Free-running removed that
constraint and the high floors then only did harm: Apollo uses `minimum_fps_target` as
`max_frametime` and **encodes a duplicate** when the game has not produced a frame in the
window, so 110/165 padded a 66fps game to ~92/s and a 77fps game to ~143/s with frames carrying
no new information — latency, not smoothness. **The throughput counters point the WRONG way**
(30/30 delivers composite 72/s vs 142/s) and it still feels better; the hold proportion is
identical either way. `COOP_VT_DERIVED_FLOORS=1` restores 110/165. See the memory note
`delivered-rate-is-not-feel`.

## The two modes have OPPOSITE hardware demands — measured 2026-08-05

Same scene, same in-game settings, both players in the same spot:

| | PC1 (RTX 4070, 12GB) | PC2 (RTX 3070, 8GB) |
|---|---|---|
| **vertical** 1920x2160 | 66 fps, GPU **55%** | 77 fps, GPU 83% |
| **horizontal** 3840x1088 | **84 fps**, GPU **84%** | **51 fps**, GPU **99%**, VRAM **7.7/7.8 GB** |

The panes hold identical pixel counts but the aspect differs enormously — 3.53:1 against 0.89:1.
With Hor+ FOV scaling the ultra-wide horizontal pane renders a far wider field of view: more
world in frustum, more draw calls, more shading. The tall vertical pane renders a narrow slice.

- **Vertical starves a strong GPU.** PC1 sits at 55% GPU and caps at 66fps — CPU/main-thread
  bound with the card idle. Horizontal's heavier scene loads it to 84% and PC1 gains 27% *while
  using less CPU* (60% vs 66%).
- **Horizontal saturates a small one.** PC2 hits 99% GPU and **7.7 of 7.8 GB VRAM** — an 8GB
  card at its hard ceiling — and drops from 77 to 51fps.

So the machine that "should" be faster only wins in horizontal. Swapping `COOP_PRIMARY` does not
change this: both panes are 3840x1088 in horizontal whichever host is primary, so the mode, not
the role, sets the demand. Left open and not worth re-chasing: PC2 may simply have a
better-binned 5600X, and SKYNET also carries the WSL2 session — see
`pc1-slower-than-pc2-investigation`.

## The trap that keeps recurring

Frame-counted constants mean different things at different rates. `coord_q_depth=12`,
`COOP_VT_CATCHUP_START=4`, `COOP_VT_CATCHUP_KEEP=2` were all tuned at 165Hz where a frame is
6ms; at 60fps the same numbers cost ~3x the milliseconds. Anything governing latency should be
expressed in TIME, not frames — see the memory note `coop-two-free-running-monitors`.
