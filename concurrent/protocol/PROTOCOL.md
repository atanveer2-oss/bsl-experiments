# Concurrent benchmarks (SetBench)

`third_party/splaylist` is Aksenov et al.'s SetBench fork. It carries every
baseline this paper must compare against — the splay-list (`flexlist_*`), the
CBTree (`cbtree*`), Bronson et al.'s concurrent AVL (`bronson_pext_bst_occ`),
and three skip lists — behind one adapter interface.

## It is not x86-only

The note formerly in `CLAUDE.md` said Linux/x86. That is wrong: with
`portability.patch` the whole tree builds and runs on **Linux/aarch64**, which
is what makes local development on Apple silicon possible. The patch is a no-op
on x86, so the same tree serves both.

```bash
docker build -t bsl-setbench experiments/setbench
docker run --rm -v "$PWD/experiments:/work" bsl-setbench /work/setbench/build.sh
```

Zero errors: 26 binaries for the fork's 13 structures (an earlier count of 40
included 14 prebuilt x86 binaries the fork ships; `build.sh` now drops them),
plus a bench and a write binary for each of ours in `ds/` (`bsl_gated`, the
paper's rule; `bsl_classic`, the control; `bsl_exact`, `bsl_morris`).
Binaries land in `setbench/.work/` and survive between runs. Validation and
the local smoke run: `RESULTS-local.md`. Locally this runs under Colima (aarch64, HVF-
accelerated, so near-native per-core) — adequate for correctness and for
single-threaded numbers, **not** for scalability claims (see below).

## Protocol

Chosen to be directly comparable to all three competing papers; where they
disagree, the choice and the reason are stated.

| | choice | why |
|---|---|---|
| distributions | uniform, 90/10, 95/5, 99/1, Zipf α=1 over shuffled keys | all three converge here; matching costs nothing |
| key range | 10⁶ dense primary, 10⁵ secondary | Aksenov '26's value; dense removes the sparse-key cache confound. 10⁵ makes the splay-list's sequential tables directly comparable |
| mixes | 100% lookup primary; 80/10/10 update | read-only is the only mix all three share, and the one our theory covers |
| timing | 20 s warmup + 20 s measured, 10 repetitions | Aksenov '26's convention; 10 reps over their 3, since 3 cannot support a difference claim |
| prefill | random insertion order, balanced-start control | matches what the published throughput numbers were measured against |
| metrics | throughput **and** path length | the structural metrics are machine-independent and are the only honest way to compare our core count against theirs |
| threads | 1, 2, 4, 8 locally; 1…T on real hardware | a prefix of the splay-list's sweep, so points line up |

Our own addition: **throughput as a function of elapsed time from a cold
structure.** The splay-list's 10 s-vs-600 s table gestures at this without
plotting it. For a paper about adaptivity that curve is the result, not the
overhead.

## What the 8-core laptop can and cannot establish

**Can**: correctness of the integration; all structural metrics (path length,
height distribution, popularity-vs-height), which are single-threaded and
machine-independent; the splay-list's single-threaded Tables 1–3 reproduced
exactly; the full distribution set; the adaptation-knob sweep.

**Cannot**: anything past 8 threads. Every interesting contention result in
this literature lives in the 20–70 thread region — CBTree serializing on
counter updates, the splay-list's claimed scalability advantage. At 8 threads
all four structures look alike and the comparison is uninformative. Also
nothing NUMA: a single-socket unified-memory part has no cross-socket
behaviour to reproduce.

**So**: single-threaded tables and structural metrics are primary evidence;
1–8 thread curves are labelled a sanity check; no scaling conclusion is drawn
from 8 cores.

## Hardware, when we get it

Free and suitable, best first:

- **CloudLab** — free, no allocation accounting. Clemson `r7525`: 2×32-core
  EPYC 7542, 64C/128T, 15 of them. Root, real NUMA. *Advisor must create the
  project; students cannot be project leaders.*
- **Chameleon Cloud** (NSF) — bare metal, root, custom kernel. CHI@TACC has
  dual EPYC 7763 (128C/256T). New projects get 20 000 node-hours for 6 months;
  a 4-hour run costs 4. Decisions in ~1 business day. *Also advisor-led.*
- **ACCESS Explore** — **a grad student may be PI**, with an advisor letter.
  400 000 credits ≈ 3 125 whole-node-hours on Purdue Anvil (2×EPYC 7763,
  128 cores). No root, but `numactl`/`taskset`/`hwloc` all work. This is the
  one that can be filed without waiting on anyone.

Paid fallback: AWS `c7i.metal-24xl` spot (48C/96T) ≈ **$25 for 20 hours**, or
`c7g.metal` (64-core Graviton) at ~$0.85/hr spot — the cheapest bare metal AWS
has, and viable precisely *because* the fork builds on arm64.

Prefer bare metal over a VM: a scalability curve that bends at 32 threads
because of a co-tenant rather than because of the algorithm is a retracted
figure.
