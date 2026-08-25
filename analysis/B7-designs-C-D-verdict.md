# B7 — written verdict on Designs C and D

Both were to be evaluated on paper (PROMPT.md §5). Neither was implemented.
Both verdicts are backed by measurements in this repo, not by preference.

---

## Design C — `dlmopen(LM_ID_NEWLM)` into a private namespace

**Verdict: rejected. Not a judgement call — it does not work.**

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
one — `dlmopen` creates a new *link map*, not a new *loader*. So the newer
`libc.so.6` is still being loaded by the older `ld-linux`, and still fails the
same version check.

**E9 measures exactly this**, and reports the same error as plain `dlopen` (E8):

```
E8  FAIL  /lib64/ld-linux-x86-64.so.2: version `GLIBC_2.35' not found (required by newglibc/libc.so.6)
E9  FAIL  /lib64/ld-linux-x86-64.so.2: version `GLIBC_2.35' not found (required by newglibc/libc.so.6)
```

Identical, byte for byte. The namespace changed nothing.

Two further objections would apply even if the version lock did not:

1. **It breaks the one-allocator rule.** A private namespace gets its own
   `malloc` arena. The Vulkan loader ↔ ICD boundary passes ownership of
   allocations in both directions (`pAllocator` callbacks, `vkAllocateMemory`,
   every `vkGet*` that fills a caller-provided buffer). Splitting the heap
   across that boundary is a use-after-free generator, not a compatibility
   layer.
2. **`RTLD_LOCAL` semantics are wrong for a driver.** An ICD must see the
   loader's symbols and vice versa; a namespace exists precisely to prevent
   that.

**Cost of pursuing it anyway:** unbounded, because the first step is
impossible. Recorded and closed.

---

## Design D — private ELF loader (Solo, mirrored)

**Verdict: not justified. It is the correct design for a problem we do not have.**

Design D means writing our own ELF loader that maps host objects without
involving `ld.so` at all, then resolving their libc imports against adapters —
which is exactly what [pg83/solo](https://github.com/pg83/solo) does.

### Why Solo needs it and we do not

Solo solves **case 3**: a *static musl* process loading *glibc* host drivers.
That process has no `ld.so` and no dynamic libc — there is nothing to ask. A
private loader is not a design choice there, it is the only option.

Our cases are 1 and 2. In both, the process is a normal glibc process with a
working `ld.so` that already implements, correctly and for free:

| Feature | Who does it for us |
|---|---|
| relocations (`RELA`, `RELR`, `IRELATIVE`) | `ld.so` |
| TLS, **including TLSDESC** | `ld.so` |
| `IFUNC` resolution | `ld.so` |
| `RELRO` re-protection | `ld.so` |
| `dl_iterate_phdr` for unwinders | `ld.so` |
| versioned symbol lookup | `ld.so` |
| `AT_SECURE`, RPATH/RUNPATH | `ld.so` |
| dependency ordering, `DF_BIND_NOW` | `ld.so` |

Design D trades all of that for roughly **2 700 lines** of loader
(`lib/elf_loader.cpp`) that must reimplement it. TLSDESC alone is a subtle,
architecture-specific piece of work, and getting it slightly wrong produces
corruption that surfaces far from the cause.

### The decisive point: it does not even avoid shims

The usual argument for Design D is "then we would not need a shim." That is
false, and Solo is the proof: alongside its loader it carries
`lib/glibc_shim.cpp`, **5 948 lines** of glibc→musl ABI bridge. A private
loader changes *who* maps the object; it does not change the fact that the
object's imports must resolve to something. You still write the adapters.

So Design D costs ~2 700 lines *and* keeps the shim.

### What it would actually buy

One thing, and it is real: **isolation**. A private loader could give the host
closure its own namespace with genuinely separate symbol resolution, which
would let a host `libstdc++` coexist with a bundled one instead of colliding.

But that is the problem the T4.2 fix already solves for the measured collision
surface, which is **three sonames** (`libGL.so.1`, `libgcc_s.so.1`,
`libstdc++.so.6` — see `analysis/A5-collision-surface.md`). Three sonames do
not justify 2 700 lines, and the isolation would re-introduce the split-heap
hazard from Design C.

### When to revisit

Design D becomes justified if, and only if, one of these is measured:

- the collision surface grows to where "bundled wins" is no longer tenable —
  i.e. the app genuinely needs both a host and a bundled copy of the same
  soname, live, at once;
- a host driver is found that cannot be loaded by `ld.so` under any rewrite,
  and the reason is loader policy rather than a missing symbol;
- the project takes on **case 3** (static musl → glibc drivers), at which
  point mirroring Solo is not optional and the answer is to *use* Solo rather
  than rewrite it.

Until one of those is on the table: **justify loudly or not at all** — and
there is nothing to justify it with today.
