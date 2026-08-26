# Fix: Auto-bind /proc when --proc-isolated/--proc-isolation is used

## Problem

`--proc-isolated` (and `--proc-isolation`) flag makes `ps x` show nothing because the Alpine rootfs ships with an **empty** `/proc` directory. Without `--bind=/proc`, the guest sees this empty directory instead of the kernel's procfs. The proc isolation filter (`hpc_handle_getdents_exit`) then finds zero entries and `ps` shows nothing.

## Root Cause

The rootfs `/proc/` is an empty directory (only `.` and `..`). proot does not auto-mount procfs. When the user specifies `--proc-isolated` without `--bind=/proc`, the guest `/proc` is useless — it's just an empty dir from the rootfs.

## Evidence

| Scenario | `--bind=/proc` | `--proc-isolated` | `ls /proc` numerics | `ps x` result |
|----------|:-:|:-:|:-:|---|
| Test 17 | no | no | 0 | no process info (expected — empty proc) |
| Test 18 | yes | no | 253 | shows ALL host processes (expected) |
| Test 19 | yes | yes | 4 | shows only proot PIDs (correct!) |
| Test 14 | no | yes | 0 | shows nothing (BUG) |

## Proposed Fix

In `cli/proot.c`, after all CLI options are parsed but before `initialize_bindings()`, check if proc isolation flags are set and `/proc` binding is NOT already in the pending list. If so, auto-add a `/proc` → `/proc` binding.

### Files to change

- `proot-source/src/cli/proot.c` — add auto-bind logic in the option parsing or config hook phase

### Implementation

Add a post-parse hook (in the `pre_initialize_bindings` config hook or right before `initialize_bindings()` call in `cli.c`) that:

1. Checks if `HpcConfig.flags & ISOLATE_PROC` is set
2. Checks if any pending binding has guest path `/proc/`
3. If no `/proc` binding exists, adds one via `insort_binding3(tracee, "/proc/", "/proc/")` with appropriate access mode

### Location in code

In `proot-source/src/cli/cli.c` around line 443 (before `initialize_bindings()`):

```c
/* Auto-bind /proc when proc isolation is active but no /proc binding exists */
{
    Extension *ext = find_extension(tracee, hpc_callback);
    if (ext != NULL) {
        HpcConfig *config = (HpcConfig *)ext->config;
        if (config != NULL && (config->flags & ISOLATE_PROC)) {
            /* Check if /proc binding already exists in pending list */
            bool has_proc_bind = false;
            Binding *b;
            CIRCLEQ_FOREACH(b, tracee->fs->bindings.pending, link.pending) {
                if (compare_paths(b->guest.path, "/proc") == PATHS_ARE_EQUAL) {
                    has_proc_bind = true;
                    break;
                }
            }
            if (!has_proc_bind) {
                insort_binding3(tracee, tracee->fs, "/proc", "/proc");
                VERBOSE(tracee, 1, "proc_isolation: auto-bound /proc (no user binding found)");
            }
        }
    }
}
```

### Alternative: simpler approach

The simpler approach is to add the auto-bind inside the `handle_proc_isolation_flag()` function in `cli/proot.c` (around line 567), right after setting the flag:

```c
static int handle_proc_isolation_flag(Tracee *tracee, unsigned int flag)
{
    /* ... existing code to set flag ... */

    /* Auto-bind /proc if not already bound */
    {
        Binding *b;
        bool found = false;
        CIRCLEQ_FOREACH(b, tracee->fs->bindings.pending, link.pending) {
            if (compare_paths(b->guest.path, "/proc") == PATHS_ARE_EQUAL) {
                found = true;
                break;
            }
        }
        if (!found) {
            insort_binding3(tracee, tracee->fs, "/proc", "/proc");
            VERBOSE(tracee, 1, "proc_isolation: auto-bound /proc for isolation");
        }
    }

    return 0;
}
```

This is cleaner because it runs at option-parse time before bindings are initialized.

## Verification

### Test 1: Auto-bind works (the main fix)
```bash
PROOT="/data/data/com.termux/files/usr/bin/proot"
ROOTFS=".../containers/alpine/rootfs"
env -i PATH=/bin:/usr/bin "$PROOT" \
  --kill-on-exit --link2symlink -L --change-id=0:0 \
  --proc-isolated \
  --rootfs="$ROOTFS" --cwd=/root --bind=/dev --bind=/sys \
  /bin/sh -c 'sleep 100 & ps x; kill $!'
```
Expected: `ps x` shows shell PID, sleep, ps — NOT empty.

### Test 2: No double-bind when user provides --bind=/proc
```bash
env -i PATH=/bin:/usr/bin "$PROOT" \
  --kill-on-exit --link2symlink -L --change-id=0:0 \
  --proc-isolated \
  --rootfs="$ROOTFS" --cwd=/root --bind=/dev --bind=/sys --bind=/proc \
  /bin/sh -c 'sleep 100 & ps x; kill $!'
```
Expected: Same as Test 1 (no duplicate binding, no error).

### Test 3: Existing pentest tests still pass
```bash
bash pentest/test_phase_c.sh
```
Expected: 6/6 PASS (or whatever the current pass count is).

### Test 4: No isolation flag → no auto-bind (zero overhead)
```bash
env -i PATH=/bin:/usr/bin "$PROOT" \
  --kill-on-exit --link2symlink -L --change-id=0:0 \
  --rootfs="$ROOTFS" --cwd=/root --bind=/dev --bind=/sys \
  /bin/sh -c 'ls /proc 2>/dev/null | head -3'
```
Expected: no auto-bind, empty `/proc` (same as before).

## Commit

```bash
git add proot-source/src/cli/proot.c
# Bump TERMUX_PKG_REVISION in packages/proot/build.sh (current: 27 → 28)
git commit -m "fix(proc): auto-bind /proc when --proc-isolated/--proc-isolation is used"
```

## Risks

- Minimal: the auto-bind only fires when proc isolation is requested AND no /proc binding exists. Zero overhead otherwise.
- The binding is `/proc` → `/proc` (identity), same as what users would manually specify.
- No impact on existing behavior when `--bind=/proc` is already provided.
