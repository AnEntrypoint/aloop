# kernel/ — PREEMPT_RT kernel + RT tuning

The real-time kernel config and the boot-time tuning that make 64-sample audio
blocks survive without xruns:
- a PREEMPT_RT-patched/enabled kernel for Pi 4,
- `cmdline.txt` isolcpus / nohz_full / rcu_nocbs for the audio cores,
- IRQ-affinity + SCHED_FIFO + mlockall tuning applied at boot.

Each knob is documented with *why* — see `docs/RT-TUNING.md` (created during the
RT step). The RT kernel is the single biggest task in the migration.
