# This fork, and who depends on it

`charlesvestal/dsp56300` carries the Move and iOS work on top of
[dsp56300/dsp56300](https://github.com/dsp56300/dsp56300). Nothing builds this
repo directly: it is reached as a submodule of `charlesvestal/gearmulator`, which
is itself pinned by five different repositories at five different commits.

So a branch here has one job — **keep a commit reachable that some repo's
gearmulator pin still names.** Two of the branches below are the only ref holding
theirs, and deleting one breaks `git submodule update` in a repo that has nothing
to say why.

| Branch | Holds | Needed by |
|---|---|---|
| `ios-move-on-sep21` | its head | `gearmulator-ios`, which tracks it rather than freezing a pin — **only ref** |
| `integration/ios-move` | `657f4004` | `schwung-jp8000` (via gearmulator `ef72c4f0`) — **only ref**. The pre-September history; superseded as a working branch, kept for this pin. |
| `ios-asmjit-bump`, `interp-dispatch-port` | `317b84d3` | `je8086-ios`, `schwung-vavra` |

`charlesvestal/gearmulator`'s `FORK.md` carries the full table and the commands
to recheck it.

## Two histories, no merge base

Upstream rewrote its history in September 2026. `integration/ios-move` predates
that and is a **three-commit orphan** against it — `git merge` refuses with
"commits don't follow merge-base", and `git cherry-pick` turns every file into an
add/add conflict.

`ios-move-on-sep21` is that work re-applied onto upstream `97530405` as patches.
Do the same on the next merge rather than fighting git over it, and run
`dsp56kTestRunner` afterwards: the September merge applied cleanly and still
regressed `UnitTests::blockOnExtensionWord`.

## What this fork changes

Platform-neutral, shared by Move and iOS:

- the decoded-opcode cache and flat interpreter dispatch (`dsp.cpp`, `dsp_ops.inl`,
  `dsp_jumptable.inl`) — this is what makes a JIT-free build usable
- `Audio`: the bounded consumer wait, and the opt-in input-backlog discard
- `memory`: `sizePCode()`, so a per-PC cache is sized by the P **code** span and
  not by the bridged range — 200 MB on a microQ otherwise
- `error.h`: the per-instruction memory-error logs compiled out of Release, worth
  2.13x on a Virus TI

Apple-specific:

- `threadtools.cpp`: a short realtime constraint window, renewed often, instead of
  the old 46 ms one
- `mmuhelper.cpp`: `DSP56K_NO_MMU=1` to force the non-MMU path
- `source/asmjit` points at a fork that includes `OSCacheControl.h` on **every**
  Apple target; without it an iOS build has no `sys_icache_invalidate`
