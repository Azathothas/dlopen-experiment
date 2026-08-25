# Verdict on the two rejected designs

Both were evaluated on paper. Neither was implemented. Both verdicts are backed
by measurements in this repo, not by preference.

---

## Private namespace: `dlmopen(LM_ID_NEWLM)`

**Verdict: rejected. Not a judgement call, it does not work.**

The idea is that a private link-map namespace gives the host library its own
copy of libc, so the version conflict disappears. It does not, for a reason
that has nothing to do with namespaces.

`libc.so.6` and `ld-linux.so` are **version-locked to each other**:

```
new libc.so.6 requires from ld-linux : GLIBC_2.2.5, GLIBC_2.3, GLIBC_2.35, GLIBC_PRIVATE
old ld-linux  defines                : GLIBC_2.2.5, GLIBC_2.3, GLIBC_2.4
```

There is exactly one `ld.so` in a process. The kernel maps it at `execve`, it
owns the thread pointer and TLS, and a private namespace does not get a second
one. `dlmopen` creates a new **link map**, not a new **loader**. So the newer
`libc.so.6` is still being loaded by the older `ld-linux`, and still fails the
same version check.

**E9 measures exactly this**, and reports the same error as plain `dlopen`
(E8):

```
E8  FAIL  /lib64/ld-linux-x86-64.so.2: version `GLIBC_2.35' not found (required by newglibc/libc.so.6)
E9  FAIL  /lib64/ld-linux-x86-64.so.2: version `GLIBC_2.35' not found (required by newglibc/libc.so.6)
```

Identical, byte for byte. The namespace changed nothing.

Two further objections would apply even if the version lock did not:

1. **It breaks the one-allocator rule.** A private namespace gets its own
   `malloc` arena. The Vulkan loader and ICD boundary passes ownership of
   allocations in both directions: `pAllocator` callbacks, `vkAllocateMemory`,
   and every `vkGet*` that fills a caller-provided buffer. Splitting the heap
   across that boundary is a use-after-free generator, not a compatibility
   layer.
2. **`RTLD_LOCAL` semantics are wrong for a driver.** An ICD must see the
   loader's symbols and the loader must see the ICD's. A namespace exists
   precisely to prevent that.

Cost of pursuing it anyway: unbounded, because the first step is impossible.
Recorded and closed.

---

## Private ELF loader, mirroring Solo

**Verdict: not justified. It is the correct design for a problem this project
does not have.**

This means writing an ELF loader that maps host objects without involving
`ld.so` at all, then resolving their libc imports against adapters. That is
exactly what [pg83/solo](https://github.com/pg83/solo) does.

### Why Solo needs it and this project does not

Solo solves the case where a **static musl** process loads **glibc** host
drivers. That process has no `ld.so` and no dynamic libc, so there is nothing to
ask. A private loader is not a design choice there, it is the only option.

The cases here are different. In both, the process is a normal glibc process
with a working `ld.so` that already implements, correctly and for free:

| Feature | Provided by |
|---|---|
| relocations: `RELA`, `RELR`, `IRELATIVE` | `ld.so` |
| TLS, **including TLSDESC** | `ld.so` |
| `IFUNC` resolution | `ld.so` |
| `RELRO` re-protection | `ld.so` |
| `dl_iterate_phdr` for unwinders | `ld.so` |
| versioned symbol lookup | `ld.so` |
| `AT_SECURE`, RPATH and RUNPATH | `ld.so` |
| dependency ordering, `DF_BIND_NOW` | `ld.so` |

A private loader trades all of that for roughly **2700 lines**
(`lib/elf_loader.cpp` in Solo) that must reimplement it. TLSDESC alone is
subtle and architecture-specific, and getting it slightly wrong produces
corruption that surfaces far from the cause.

### The decisive point: it does not even avoid shims

The usual argument is "then we would not need a shim". That is false, and Solo
is the proof. Alongside its loader it carries `lib/glibc_shim.cpp`, **5948
lines** of glibc-to-musl ABI bridge. A private loader changes **who** maps the
object; it does not change the fact that the object's imports must resolve to
something. The adapters still have to be written.

So this design costs about 2700 lines **and** keeps the shim.

### What it would actually buy

One thing, and it is real: **isolation**. A private loader could give the host
closure its own namespace with genuinely separate symbol resolution, letting a
host `libstdc++` coexist with a bundled one instead of colliding.

But that is the problem the "bundled wins" fix already solves for the measured
collision surface, which is **three sonames**: `libGL.so.1`, `libgcc_s.so.1`
and `libstdc++.so.6` (see [ground-truth.md](ground-truth.md)). Three sonames do
not justify 2700 lines, and the isolation would reintroduce the split-heap
hazard from the private-namespace design above.

### When to revisit

This becomes justified if, and only if, one of these is measured:

- the collision surface grows to where "bundled wins" is no longer tenable,
  meaning the app genuinely needs both a host and a bundled copy of the same
  soname, live, at the same time;
- a host driver is found that `ld.so` cannot load under any rewrite, and the
  reason is loader policy rather than a missing symbol;
- the project takes on the static-musl case, at which point mirroring Solo is
  not optional and the right answer is to **use** Solo rather than rewrite it.

Until one of those is on the table there is nothing to justify it with.
