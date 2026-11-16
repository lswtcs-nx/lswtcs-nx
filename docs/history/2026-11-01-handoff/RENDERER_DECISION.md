# Mesa or Deko

The main route is improving the working Mesa renderer while keeping the
incomplete Deko work available.

## Avoid two unsupported conclusions

"Mesa is a huge translation layer, so it must be the bottleneck" is not a
measurement. This port already has a native PM4-to-GL bridge. Its total cost also
includes recompiled game code, services, shader generation, recording, uploads,
driver validation, GPU work and scheduling/presentation waits. The largest
removable steady cost has not been fully attributed.

"Deko removes shader compilation" is also incomplete. Native GPU code must still
be produced; the prototype uses host/AOT preparation. Deko can change the API/
driver path, but does not automatically repair guest-memory corruption, decoder
errors, input snapshots, alias semantics or ownership races.

## What is established

- Mesa has physical gameplay/native-save evidence and remains the delivery path.
- Deko changing-frame capture replay has physical evidence, and selected frozen
  frames have emulator parity results. Those are bounded renderer tests.
- No accepted full live Deko game, comparable stock-clock performance result or
  completed production EDRAM/resource-ownership foundation is claimed.
- Community references show useful fenced retirement, input validation, profiling,
  cache and wait patterns. Many use different frontends, Renut/Deko, NVK/Vulkan,
  platforms, quality settings or clocks. They are not a TCS API A/B benchmark.

## Reviewing Deko

A Deko review should be **bounded and independent**, agreed before it starts:
identify one unfinished attachment/resolve/producer/lifetime contract in the
existing PM4/shared model, compare an applicable reference, provide a minimal
falsifier and describe reuse/cost. Keep Mesa fallback and existing semantics.

A frontend replacement, general resource framework, Renut adoption or new shader
compiler/toolchain is not the default first task. Any adoption proposal needs
TCS-specific evidence and an explicit scope decision. Do not start competing
renderer migrations on the same files.

The supplied reference review records its26October source pins and limitations;
it is not a claim about the newest releases as of today.
