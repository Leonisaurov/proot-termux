# Phase D — Performance Implementation Plan (v2)

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Optimize proot's hot paths: BPF filter construction, syscall interception overhead, tracee lookup, and binding resolution.

**Architecture:** Phased approach — safe low-risk fixes first (D3, D2: remove unnecessary FILTER_SYSEXIT), then higher-impact but riskier optimizations (D1: BPF binary search, D5: tracee hash table, D6: binding cache). D4 (socket) DEFERRED — exit handler depends on FILTER_SYSEXIT. D7 (canonicalize cache) deferred due to coherence risks.

**Tech Stack:** C (proot source), seccomp BPF, ptrace, talloc allocator

**Lessons from Phase C incorporated:**
1. **Option parsing order**: Don't create resources in handlers that depend on flags parsed later.
2. **Guest vs host paths**: Always use guest paths for access mode checks.
3. **Build-test cycle**: Rebuild with `scripts/build-native.sh -c -i --skip-package` after every code change.
4. **env -i requirement**: proot needs clean environment. All tests must use `env -i PATH=/bin:/usr/bin`.
5. **Android filesystem**: /system is read-only. Use writable paths for write tests.

---

## CRITICAL FINDINGS FROM CODE REVIEW

### D4 (socket): CANNOT remove FILTER_SYSEXIT

The socket EXIT handler (exit.c:764-785) records fake netlink fds. This handler ONLY runs when FILTER_SYSEXIT is set. The enter handler sets `pending_fake_netlink_socket = true`, but without FILTER_SYSEXIT, the exit handler never fires. **DEFER D4.**

### D2 (ioctl): CAN remove FILTER_SYSEXIT — with dynamic request

The ioctl EXIT handler (exit.c:757-762) fixes FICLONE EACCES→EOPNOTSUPP. We can set `tracee->sysexit_pending = true` in the ENTER handler when `cmd == _IOW(0x94, 9, int)`. The event loop (event.c:632-642) checks `sysexit_pending` and restarts with PTRACE_SYSCALL if set.

### D3 (faccessat2): SAFE to remove

No exit handler for faccessat2 exists in exit.c. fake_id0 handles it in the enter path. **Safe to remove FILTER_SYSEXIT.**

### D5 (hash table): Lifecycle critical

`free_terminated_tracees()` (tracee.c:368-379) calls `TALLOC_FREE(tracee)` which triggers destructors. The hash entry MUST be removed BEFORE the tracee is freed, or use a talloc destructor on the hash entry itself.

### D1 (BPF binary search): sysnums is const-like

The `sysnums` parameter to `set_seccomp_filters` is a pointer to a static array. Creating a sorted copy requires talloc allocation. The sorted copy must be freed after use.

---

## Task 1: Remove FILTER_SYSEXIT from PR_faccessat2 (D3)

**Objective:** Remove unnecessary FILTER_SYSEXIT flag from faccessat2 — no exit handler depends on it.

**Files:**
- Modify: `proot-source/src/syscall/seccomp.c:356`

**Step 1: Locate and remove**

Find:
```c
{ PR_faccessat2,	FILTER_SYSEXIT },
```

Change to:
```c
{ PR_faccessat2,	0 },
```

**Step 2: Verify compilation**

Run: `cd proot-source/src && clang -fsyntax-only -I. -I.. -D_GNU_SOURCE -DVERSION=\"5.1.107.87\" -DARG_MAX=131072 syscall/seccomp.c`
Expected: no errors

**Step 3: Build and test**

Run: `cd /path/to/proot-termux && bash scripts/build-native.sh -c -i --skip-package`

Test access() works:
```bash
env -i PATH=/bin:/usr/bin PROOT_L2S_DIR=/tmp /path/to/proot \
  --kill-on-exit --link2symlink -L --change-id=0:0 \
  --rootfs=/path/to/rootfs --cwd=/root --bind=/dev --bind=/sys \
  /bin/sh -c 'test -r /etc/hosts && echo ACCESS_OK || echo ACCESS_FAIL'
```
Expected: ACCESS_OK

**Step 4: Commit**
```bash
git add proot-source/src/syscall/seccomp.c
git commit -m "perf(seccomp): remove FILTER_SYSEXIT from faccessat2 (D3)"
```

---

## Task 2: Remove FILTER_SYSEXIT from PR_ioctl — dynamic for FICLONE (D2)

**Objective:** Remove static FILTER_SYSEXIT from ioctl; dynamically request sysexit only for FICLONE.

**Files:**
- Modify: `proot-source/src/syscall/seccomp.c:368` (remove FILTER_SYSEXIT)
- Modify: `proot-source/src/syscall/enter.c:2245` (add FICLONE detection + sysexit request)

**Step 1: Remove static FILTER_SYSEXIT from seccomp.c**

Find (inside `#ifdef __ANDROID__`):
```c
{ PR_ioctl,		FILTER_SYSEXIT },
```

Change to:
```c
{ PR_ioctl,		0 },
```

**Step 2: Add dynamic sysexit request in enter.c**

In the ioctl enter handler (line 2245), after the existing cmd checks, add FICLONE detection:

```c
case PR_ioctl: {
    word_t cmd = peek_reg(tracee, CURRENT, SYSARG_2);
    word_t arg = peek_reg(tracee, CURRENT, SYSARG_3);

    /* ... existing SIOCGIFINDEX and terminal ioctl handling ... */

    /* D2: Request sysexit for FICLONE only — the exit handler
     * fixes EACCES→EOPNOTSUPP for copy_file_range emulation. */
    if (cmd == _IOW(0x94, 9, int) /* FICLONE */) {
        tracee->sysexit_pending = true;
    }

    break;
}
```

**IMPORTANT**: The `_IOW(0x94, 9, int)` macro must be available. Check `<sys/ioctl.h>` is included. If not, define the constant directly:
```c
#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif
```

**Step 3: Verify compilation and build**

Same pattern as Task 1.

**Step 4: Test ioctl still works**

```bash
env -i PATH=/bin:/usr/bin PROOT_L2S_DIR=/tmp /path/to/proot \
  --kill-on-exit --link2symlink -L --change-id=0:0 \
  --rootfs=/path/to/rootfs --cwd=/root --bind=/dev --bind=/sys \
  /bin/sh -c 'ls -la /dev/null && echo IOCTL_OK'
```
Expected: IOCTL_OK

**Step 5: Commit**
```bash
git add proot-source/src/syscall/seccomp.c proot-source/src/syscall/enter.c
git commit -m "perf(seccomp): remove FILTER_SYSEXIT from ioctl, dynamic for FICLONE (D2)"
```

---

## Task 3: BPF Binary Search — Sort sysnums (D1 prep)

**Objective:** Sort proot_sysnums by syscall number to enable binary search in BPF filter construction.

**Files:**
- Modify: `proot-source/src/syscall/seccomp.c` (add sort + binary search helpers)

**Step 1: Add sort helper**

Add before `set_seccomp_filters()`:
```c
/**
 * Compare two FilteredSysnum entries by value (for qsort).
 */
static int compare_sysnum(const void *a, const void *b)
{
    const FilteredSysnum *sa = (const FilteredSysnum *)a;
    const FilteredSysnum *sb = (const FilteredSysnum *)b;
    if (sa->value < sb->value) return -1;
    if (sa->value > sb->value) return 1;
    return 0;
}

/**
 * Sort a FilteredSysnum array by value in-place.
 * Array must be terminated with { PR_void, 0 }.
 */
static void sort_filtered_sysnums(FilteredSysnum *sysnums)
{
    size_t count = 0;
    while (sysnums[count].value != PR_void)
        count++;
    if (count > 1)
        qsort(sysnums, count, sizeof(FilteredSysnum), compare_sysnum);
}
```

**Step 2: Add binary search helper**

```c
/**
 * Binary search for a syscall value in a sorted FilteredSysnum array.
 * Returns true if found with matching flag, false otherwise.
 */
static bool sorted_sysnums_contains(const FilteredSysnum *sysnums,
                                     word_t value, int *flag_out)
{
    size_t low = 0, high = 0;
    while (sysnums[high].value != PR_void)
        high++;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (sysnums[mid].value == value) {
            if (flag_out != NULL)
                *flag_out = sysnums[mid].flag;
            return true;
        }
        if (sysnums[mid].value < value)
            low = mid + 1;
        else
            high = mid;
    }
    return false;
}
```

**Step 3: Verify compilation**

Same pattern as Task 1.

**Step 4: Commit**
```bash
git add proot-source/src/syscall/seccomp.c
git commit -m "perf(seccomp): add sorted sysnums and binary search helper (D1 prep)"
```

---

## Task 4: BPF Binary Search — Use in Filter Construction (D1)

**Objective:** Replace linear scan with binary search in BPF filter construction.

**Files:**
- Modify: `proot-source/src/syscall/seccomp.c:253-300` (set_seccomp_filters)

**Step 1: Create sorted copy in set_seccomp_filters**

At the start of `set_seccomp_filters()`, after `new_program_filter`:
```c
/* D1: Create sorted copy of sysnums for binary search */
size_t sysnums_count = 0;
FilteredSysnum *sorted_sysnums;
while (sysnums[sysnums_count].value != PR_void)
    sysnums_count++;
sorted_sysnums = talloc_array(program.filter, FilteredSysnum, sysnums_count + 1);
if (sorted_sysnums == NULL) {
    status = -ENOMEM;
    goto end;
}
memcpy(sorted_sysnums, sysnums, (sysnums_count + 1) * sizeof(FilteredSysnum));
sort_filtered_sysnums(sorted_sysnums);
```

**Step 2: Replace linear scan with binary search**

In the pre-computation loop (line 274-280), replace:
```c
for (k = 0; sysnums[k].value != PR_void; k++) {
    syscall = detranslate_sysnum(seccomp_archs[i].abis[j], sysnums[k].value);
    if (syscall != SYSCALL_AVOIDER)
        nb_traced_syscalls++;
}
```

With:
```c
for (k = 0; sorted_sysnums[k].value != PR_void; k++) {
    syscall = detranslate_sysnum(seccomp_archs[i].abis[j], sorted_sysnums[k].value);
    if (syscall != SYSCALL_AVOIDER)
        nb_traced_syscalls++;
}
```

And in the filter construction loop (line 287-290), replace:
```c
for (k = 0; sysnums[k].value != PR_void; k++) {
    syscall = detranslate_sysnum(seccomp_archs[i].abis[j], sysnums[k].value);
    if (syscall != SYSCALL_AVOIDER) {
        status = add_trace_syscall(&program, syscall, sysnums[k].flag);
```

With:
```c
for (k = 0; sorted_sysnums[k].value != PR_void; k++) {
    syscall = detranslate_sysnum(seccomp_archs[i].abis[j], sorted_sysnums[k].value);
    if (syscall != SYSCALL_AVOIDER) {
        status = add_trace_syscall(&program, syscall, sorted_sysnums[k].flag);
```

**Step 3: Free sorted copy at end**

In the `end:` label, add:
```c
TALLOC_FREE(sorted_sysnums);
```

**Step 4: Verify compilation and build**

Same pattern as Task 1.

**Step 5: Stress test**

```bash
env -i PATH=/bin:/usr/bin PROOT_L2S_DIR=/tmp /path/to/proot \
  --kill-on-exit --link2symlink -L --change-id=0:0 \
  --rootfs=/path/to/rootfs --cwd=/root --bind=/dev --bind=/sys \
  /bin/sh -c 'for i in $(seq 1 100); do cat /etc/hosts > /dev/null; done; echo STRESS_OK'
```
Expected: STRESS_OK

**Step 6: Commit**
```bash
git add proot-source/src/syscall/seccomp.c
git commit -m "perf(seccomp): use binary search in BPF filter construction (D1)"
```

---

## Task 5: Tracee Hash Table (D5) — Add to tracee.h

**Objective:** Add hash table data structure for O(1) tracee lookup.

**Files:**
- Modify: `proot-source/src/tracee/tracee.h` (add hash table types)

**Step 1: Add hash table types**

After the `Tracees` typedef:
```c
/* D5: Hash table for O(1) tracee lookup by pid */
#define TRACEE_HASH_SIZE 64  /* Must be power of 2 */

typedef struct tracee_hash_entry {
    pid_t pid;
    struct tracee *tracee;
    struct tracee_hash_entry *next;
} TraceeHashEntry;

typedef struct {
    TraceeHashEntry *buckets[TRACEE_HASH_SIZE];
    int count;
} TraceeHashTable;
```

**Step 2: Verify compilation**

Same pattern as Task 1.

**Step 3: Commit**
```bash
git add proot-source/src/tracee/tracee.h
git commit -m "perf(tracee): add hash table types for O(1) pid lookup (D5 prep)"
```

---

## Task 6: Tracee Hash Table — Implement in tracee.c

**Objective:** Implement hash table operations and integrate into tracee lifecycle.

**Files:**
- Modify: `proot-source/src/tracee/tracee.c` (add hash ops, integrate into get_tracee/new_tracee/free)

**Step 1: Add hash table globals and functions**

Add after the tracees list definition:
```c
/* D5: Hash table for O(1) tracee lookup */
static TraceeHashTable tracee_hash = { .count = 0 };

static inline unsigned int tracee_hash_pid(pid_t pid)
{
    return ((unsigned int)pid) & (TRACEE_HASH_SIZE - 1);
}

static void tracee_hash_insert(pid_t pid, Tracee *tracee)
{
    unsigned int idx = tracee_hash_pid(pid);
    TraceeHashEntry *entry = talloc(tracee, TraceeHashEntry);
    if (entry == NULL) return;
    entry->pid = pid;
    entry->tracee = tracee;
    entry->next = tracee_hash.buckets[idx];
    tracee_hash.buckets[idx] = entry;
    tracee_hash.count++;
}

static void tracee_hash_remove(pid_t pid)
{
    unsigned int idx = tracee_hash_pid(pid);
    TraceeHashEntry **pp = &tracee_hash.buckets[idx];
    while (*pp) {
        if ((*pp)->pid == pid) {
            TraceeHashEntry *tmp = *pp;
            *pp = tmp->next;
            TALLOC_FREE(tmp);
            tracee_hash.count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

static Tracee *tracee_hash_find(pid_t pid)
{
    unsigned int idx = tracee_hash_pid(pid);
    TraceeHashEntry *entry = tracee_hash.buckets[idx];
    while (entry) {
        if (entry->pid == pid)
            return entry->tracee;
        entry = entry->next;
    }
    return NULL;
}
```

**Step 2: Integrate into get_tracee**

Replace the linear scan in `get_tracee()` (line 336-344):
```c
/* Before: */
LIST_FOREACH(tracee, &tracees, link) {
    if (tracee->pid == pid) {
        TALLOC_FREE(tracee->ctx);
        tracee->ctx = talloc_new(tracee);
        return tracee;
    }
}

/* After: */
tracee = tracee_hash_find(pid);
if (tracee != NULL) {
    TALLOC_FREE(tracee->ctx);
    tracee->ctx = talloc_new(tracee);
    return tracee;
}
```

**Step 3: Integrate into new_tracee**

In `new_tracee()`, after adding to the linked list:
```c
tracee_hash_insert(tracee->pid, tracee);
```

**Step 4: Integrate into free_terminated_tracees**

CRITICAL: Remove from hash BEFORE talloc_free:
```c
void free_terminated_tracees()
{
    Tracee *next;
    next = tracees.lh_first;
    while (next != NULL) {
        Tracee *tracee = next;
        next = tracee->link.le_next;
        if (tracee->terminated) {
            tracee_hash_remove(tracee->pid);  /* D5: remove from hash first */
            TALLOC_FREE(tracee);
        }
    }
}
```

**Step 5: Verify compilation and build**

Same pattern as Task 1.

**Step 6: Stress test**

```bash
env -i PATH=/bin:/usr/bin PROOT_L2S_DIR=/tmp /path/to/proot \
  --kill-on-exit --link2symlink -L --change-id=0:0 \
  --rootfs=/path/to/rootfs --cwd=/root --bind=/dev --bind=/sys \
  /bin/sh -c 'for i in $(seq 1 50); do sleep 0.1 & done; wait; echo HASH_OK'
```
Expected: HASH_OK

**Step 7: Commit**
```bash
git add proot-source/src/tracee/tracee.c
git commit -m "perf(tracee): integrate hash table into get_tracee (D5)"
```

---

## Task 7: Binding MRU Cache (D6)

**Objective:** Add MRU cache for binding lookups.

**Files:**
- Modify: `proot-source/src/path/binding.c` (add cache, integrate into get_binding)

**Step 1: Add MRU cache structure**

Add at top of binding.c (after includes):
```c
/* D6: MRU cache for binding lookups */
#define BINDING_CACHE_SIZE 4
static struct {
    char path[PATH_MAX];
    Side side;
    Binding *binding;
} binding_cache[BINDING_CACHE_SIZE];
static int binding_cache_idx = 0;

static void binding_cache_invalidate(void)
{
    memset(binding_cache, 0, sizeof(binding_cache));
    binding_cache_idx = 0;
}
```

**Step 2: Add cache check at start of get_binding**

```c
Binding *get_binding(const Tracee *tracee, Side side, const char path[PATH_MAX])
{
    Binding *binding;
    size_t path_length = strlen(path);
    int i;

    /* D6: check MRU cache first */
    for (i = 0; i < BINDING_CACHE_SIZE; i++) {
        if (binding_cache[i].binding != NULL
            && binding_cache[i].side == side
            && strcmp(binding_cache[i].path, path) == 0) {
            return binding_cache[i].binding;
        }
    }

    /* ... existing linear search code ... */

    /* D6: update cache with result */
    if (binding != NULL) {
        strncpy(binding_cache[binding_cache_idx].path, path, PATH_MAX - 1);
        binding_cache[binding_cache_idx].path[PATH_MAX - 1] = '\0';
        binding_cache[binding_cache_idx].side = side;
        binding_cache[binding_cache_idx].binding = binding;
        binding_cache_idx = (binding_cache_idx + 1) % BINDING_CACHE_SIZE;
    }

    return binding;
}
```

**Step 3: Invalidate cache on ALL binding mutations**

Add `binding_cache_invalidate();` at the START of:
- `insort_binding2()` (line ~390)
- `remove_binding_from_all_lists()` (line ~175)
- Any other function that modifies the binding lists

**CRITICAL**: Search for ALL callers that modify bindings. Miss one = stale cache = wrong behavior.

**Step 4: Verify compilation and build**

Same pattern as Task 1.

**Step 5: Benchmark**

```bash
env -i PATH=/bin:/usr/bin PROOT_L2S_DIR=/tmp /path/to/proot \
  --kill-on-exit --link2symlink -L --change-id=0:0 \
  --rootfs=/path/to/rootfs --cwd=/root --bind=/dev --bind=/sys \
  /bin/sh -c 'time find /etc -type f 2>/dev/null | wc -l'
```

**Step 6: Commit**
```bash
git add proot-source/src/path/binding.c
git commit -m "perf(binding): add MRU cache for get_binding lookups (D6)"
```

---

## Task 8: Build, Test, Update FIXES.md

**Objective:** Full build, comprehensive testing, documentation update.

**Step 1: Full rebuild**

```bash
cd /path/to/proot-termux && bash scripts/build-native.sh -c -i
```

**Step 2: Run all pentest scripts**

```bash
cd /path/to/proot-termux
for test in pentest/test_b*.sh pentest/test_phase_c_final.sh; do
    echo "=== $test ==="
    timeout 60 bash "$test" 2>&1
done
```

**Step 3: Update FIXES.md**

Mark D1, D2, D3, D5, D6 as completed. Note D4 deferred.

**Step 4: Bump TERMUX_PKG_REVISION**

Edit `packages/proot/build.sh`, change `TERMUX_PKG_REVISION=22` to `TERMUX_PKG_REVISION=23`.

**Step 5: Final commit**

```bash
git add -A
git commit -m "perf: Phase D — BPF binary search, remove FILTER_SYSEXIT, hash tracee lookup, binding cache (REV 23)"
```

---

## Risks and Mitigations

| Risk | Severity | Mitigation |
|------|----------|------------|
| D1: Binary search breaks BPF on some kernels | CRITICAL | Test on CI; sorted copy is temporary, original untouched |
| D2: FICLONE constant not available | MEDIUM | Define fallback `#ifndef FICLONE` |
| D5: Hash entry lifetime vs tracee lifetime | HIGH | Hash entry allocated under tracee talloc ctx; freed with tracee |
| D5: free_terminated_tracees race | HIGH | Remove from hash BEFORE TALLOC_FREE |
| D6: Cache stale after binding mutation | HIGH | Invalidate in ALL mutation functions; search exhaustively |
| D7: Canonicalize cache coherence | TOO HIGH | DEFERRED |

## Deferred Items

| ID | Reason |
|----|--------|
| D4 (socket FILTER_SYSEXIT) | Exit handler depends on FILTER_SYSEXIT; dynamic request not possible without modifying event loop |
| D7 (canonicalize cache) | Coherence risks outweigh benefits; rename/rmdir/unlink/mkdir invalidation complex |
