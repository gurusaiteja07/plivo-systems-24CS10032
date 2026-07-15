# RUNLOG.md

## Important note on test profiles

`profiles/A.json` and `profiles/B.json` were **not included** in the assignment
bundle handed to this harness — `relay.py` says the grading profiles are
"ones you have not seen." To generate real numbers for this log, two local
test profiles were authored to stand in for them:

- **A (mild):** `loss=0.03`, `delay_min/max_ms=5/25`, `dup=0.01` — steady
  random loss, no bursts.
- **B (hostile):** `loss=0.02`, `delay_min/max_ms=5/35`, `dup=0.005`,
  `spike` (2% chance of +80ms), `burst_loss` (Gilbert-Elliott, `p_enter=0.02`,
  `p_exit=0.3`, `p_loss_in_burst=0.5`) — bursty, correlated loss on top of
  jitter spikes.

All numbers below are from actual `make && python3 run.py` runs against these
two profiles with the architecture in `sender.c`/`receiver.c` (group-of-4 XOR
FEC + paced/bounded NACK retransmit). Re-run against the real grading
profiles before submitting; the qualitative conclusions (FEC handles isolated
loss for free, NACK handles the rest, bursts are the failure mode) should
carry over even if the exact optimal `delay_ms` shifts.

Architecture did not change between runs in this log — all runs use the
single design described in `NOTES.md`/`SUMMARY.html`. The log below is a
**parameter sweep** (varying `--delay_ms`) to find, per profile, the lowest
delay that reliably clears the cap.

---

## Profile A (mild, steady loss)

### Sweep, duration=8s (400 frames) to locate the region of interest

| delay_ms | misses | miss % | overhead | result |
|---|---|---|---|---|
| 40 | 9/400 | 2.25% | 1.30x | INVALID |
| 60 | 7/400 | 1.75% | 1.30x | INVALID |
| 65 | 6/400 | 1.50% | 1.30x | INVALID |
| 70 | 7/400 | 1.75% | 1.30x | INVALID |
| 75 | 4/400 | 1.00% | 1.30x | VALID |
| 80 | 4/400 | 1.00% | 1.30x | VALID |

Reasoning: at short delay the receiver's NACK round-trip (relay adds 5-25ms
each way, plus the paced 10ms initial NACK wait) doesn't fit before the
playout deadline, so any group with 2+ losses (FEC can't fix) or an isolated
loss discovered too late to NACK counts as a miss. The threshold where misses
first clear 1% sits around 75ms for this profile — but a single 8s/400-frame
sample is noisy (each miss is a 0.25% step), so this was **not** taken as
final; see full-duration confirmation below.

### Confirmation, duration=30s (1500 frames), multiple seeds, delay_ms=80

| seed | misses | miss % | overhead | result |
|---|---|---|---|---|
| 1 | 12/1500 | 0.80% | 1.30x | VALID |
| 2 | 8/1500 | 0.53% | 1.30x | VALID |

Still close to the cap on seed 1, so added margin and re-tested at 90ms.

### Confirmation, duration=30s (1500 frames), multiple seeds, delay_ms=90

| seed | misses | miss % | overhead | result |
|---|---|---|---|---|
| 1 | 9/1500 | 0.60% | 1.30x | VALID |
| 2 | 3/1500 | 0.20% | 1.30x | VALID |
| 3 | 5/1500 | 0.33% | 1.30x | VALID |

**Chosen operating point for Profile A: `delay_ms=90`.** Consistently valid
with headroom (0.2–0.6% vs the 1.0% cap) across three seeds. Overhead is a
flat 1.30x (1 parity packet per 4 data packets = +25%, plus a small,
NACK-driven retransmit tail), well under the 2.0x cap and not the binding
constraint at any delay tested.

No architectural change was made across this sweep — only `--delay_ms` was
varied — because the miss curve was already monotonically improving with
delay and flattening out with overhead far from its cap. The only "change"
was picking the delay operating point.

---

## Profile B (hostile, bursty loss)

### Sweep, duration=10s (500 frames) to locate the region of interest

| delay_ms | misses | miss % | overhead | result |
|---|---|---|---|---|
| 100 | 10/500 | 2.00% | 1.32x | INVALID |
| 110 | 6/500 | 1.20% | 1.35x | INVALID |
| 115 | 4/500 | 0.80% | 1.34x | VALID |
| 120 | 7/500 | 1.40% | 1.34x | INVALID |
| 130 | 5/500 | 1.00% | 1.34x | VALID |
| 160 | 5/500 | 1.00% | 1.34x | VALID |
| 200 | 5/500 | 1.00% | 1.34x | VALID |

This short-sample sweep looked encouraging (valid from ~115-130ms up), but
10s/500 frames isn't enough to see a full burst-loss cycle reliably, so it
was re-checked at full duration.

### Confirmation, duration=30s (1500 frames), multiple seeds, delay_ms=150

| seed | misses | miss % | overhead | result |
|---|---|---|---|---|
| 1 | 20/1500 | 1.33% | 1.34x | INVALID |
| 2 | 19/1500 | 1.27% | 1.34x | INVALID |
| 3 | 10/1500 | 0.67% | 1.34x | VALID |

Two of three seeds failed — the short sweep above had under-sampled burst
events. Pushed delay higher.

### Confirmation, duration=30s (1500 frames), multiple seeds, delay_ms=200/250/300/400

| delay_ms | seed | misses | miss % | result |
|---|---|---|---|---|
| 200 | 1 | 14/1500 | 0.93% | VALID |
| 200 | 2 | 26/1500 | 1.73% | INVALID |
| 250 | 1 | 20/1500 | 1.33% | INVALID |
| 250 | 2 | 17/1500 | 1.13% | INVALID |
| 300 | 1 | 14/1500 | 0.93% | VALID |
| 300 | 2 | 17/1500 | 1.13% | INVALID |
| 400 | 1 | 20/1500 | 1.33% | INVALID |
| 400 | 2 | 17/1500 | 1.13% | INVALID |
| 400 | 3 | 10/1500 | 0.67% | VALID |

**Key finding: increasing `delay_ms` past ~150-200ms stops helping on
Profile B.** The miss rate floors around 0.7-1.7% and stays there all the
way out to 400ms — it does not converge below the 1% cap reliably no matter
how much playout delay is budgeted. This is a real limitation of the current
design, not a tuning problem, and no architectural change was attempted
mid-sweep for this log because the sweep itself was the diagnostic: it shows
the miss floor is delay-independent, i.e. a structural recovery-capacity
problem rather than a "give the NACK more time" problem. See `NOTES.md` for
the diagnosis (burst-correlated loss can take out a group's data, its
parity, *and* the retransmission(s), since NACK/RETX travel the same lossy
path as the original data — no amount of extra deadline time helps if the
recovery packets themselves keep getting dropped by the same burst).

**Chosen operating point for Profile B: `delay_ms=200`**, offered as a
best-effort value (lowest delay in the sweep that was VALID at least as often
as not) rather than a guaranteed-valid one — Profile B, as authored, is
capable of correlated loss bursts this architecture cannot fully cover at any
delay. This should be read as a stress profile that reveals the design's
failure mode, not as a profile the current architecture reliably passes.

---

## Summary of chosen operating points

| Profile | delay_ms | Typical miss % | Typical overhead | Reliable? |
|---|---|---|---|---|
| A (mild, steady loss) | 90 | 0.2–0.6% | 1.30x | Yes, all seeds tested |
| B (bursty/hostile) | 200 | 0.9–1.7% | 1.34x | No — best-effort only, see above |