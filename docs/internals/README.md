# Internals

Reference for contributors changing nocopy's C code, and for the tools they work with. These pages are dense on purpose:
they record every invariant and measurement. Users want [the docs](../README.md) instead.

- [Architecture: internals](architecture.md): code layers, memory layout, locking, eviction, the read path, the RESP
  tier, and the invariants every change must keep
- [Design log](design-decisions.md): every decision with its numbers, and the measured ideas that were dropped
- [Status log](status.md): what was verified when, with which commands and results, and the detailed roadmap
- [Benchmark log](benchmarks.md): every benchmark run in full: setup, dates, all configurations and caveats
- [Refactoring plan](refactoring-plan.md): the 2026-11-13 survey against the software-craft books, what was done, measured, rejected and left alone

Day-to-day commands: [Develop, test and benchmark](../how-to/develop.md) and [`./dev` commands](../reference/dev-commands.md).
