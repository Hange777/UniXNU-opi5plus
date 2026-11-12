# Evidence in this repository

`scripts/porting-record.py check` resolves every `evidence` reference in `porting.toml` relative to this
repository, so the documents a record entry points at have to exist here.

`m5/` holds the display documents referenced by the display area:

| document | what it settles |
| --- | --- |
| `M5-15-native-resolution-compositor-frames.md` | the compositor's frames really reach the screen at 2560x1440: three swaps, three 14745600-byte copies with status 0, three VOP2 flips |
| `M5-16-default-surface-lookup.md` | where the original family keeps the default framebuffer surface (+5656), which slot creates it (299, not 316), and the byte at +6016 that gates it |
| `M5-17-display-memory-region-required.md` | the surface must carry a display memory region (`PurpleGfxMem`, the alias of a `/vram` node); a region-less surface faults at `0xfffffff00578d110` in `layout_buffers` |
| `M5-18-vram-region-campaign.md` | the archive publishes `/vram` itself, `fromPath("/vram")` answers 0 in both planes for three placements, and the region table is filled directly instead |

These are English summaries with addresses, offsets and log lines; they contain no firmware, no Apple
binaries, no logs and no host paths. The authoritative copies for authoring live with the project's evidence
pack; the copies here are what the porting record validates.

`round2-result.md`, `round3-result.md` and `HANDOFF.md` are the current state of the port:

| document | what it settles |
| --- | --- |
| `round2-result.md` | the round-2 boot in one page: the kernel, payload and command line, the numbers against round 1, the Data volume's directory-owner root cause, the three things that still do not work, and what the record does not claim |
| `round3-result.md` | the round-3 boot: the one-argument difference from round 2, the injected reports the kernel accepts (4,830 reports, 0 fails), the frame timeline that turns into the home screen, the round-2 comparison, the negative results, the judgements this round retires, and what it does not claim |
| `HANDOFF.md` | the starting point for the next session: current state, the closed root cause, the open items with a next step each, the build/push/capture commands, and the traps that already cost time |

`round3-baseline/` holds the build identity of the round-3 payload, which carries the injector; `round3-result.md`
is what that payload did on the board. The boot log both pages cite (`logs/boot-round3.log`) lives in the project
working tree and is not committed, as `AGENTS.md` requires.

