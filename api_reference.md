# USRL Unified Facade API (`usrl_api.c`)

This document is a copyable API reference for the unified facade implemented in `usrl_api.c` (publisher/subscriber/context lifecycle + SHM-backed topic attach/create + health reporting).

It relies on POSIX shared memory (`shm_open`) and uses `fstat()` to discover the SHM object size before mapping so the `mmap()`/`munmap()` lengths match the real object size. [web:15][web:9]  
Micro-sleeps (`usleep`) are cooperative and may oversleep depending on scheduler/timer resolution, so backoff loops are best-effort rather than hard real-time. [web:7][web:10]

---

## Public-facing objects

### `usrl_ctx_t`
Opaque process context created by `usrl_init()` and freed by `usrl_shutdown()`.

### `usrl_pub_t`
Opaque publisher handle for one topic, bound to one SHM mapping.

### `usrl_sub_t`
Opaque subscriber handle for one topic, bound to one SHM mapping.

---

## Global defaults

### `void usrl_set_default_shm_size_mb(uint32_t mb)`
Sets a process-global minimum SHM size (in MB) used when creating topics via `usrl_pub_create()`.

**Rules**
- `mb < 8` is clamped to `8`.
- The final requested SHM size is `max(default_min_bytes, computed_ring_bytes)`.

**Nuance**
- This is a global static; it affects all subsequent publisher creations in the same process.

---

## Lifecycle

### `usrl_ctx_t *usrl_init(const usrl_sys_config_t *config)`
Initializes logging and allocates a context.

**Inputs**
- `config` (required)
  - `config->log_file_path`: forwarded to logging init.
  - `config->log_level`: forwarded to logging init.
  - `config->app_name`: copied into `ctx->name` (64 bytes including NUL).

**Returns**
- Non-NULL `usrl_ctx_t*` on success.
- `NULL` if `config==NULL` or allocation fails.

**Nuances**
- Logging is initialized before `ctx` allocation; if allocation fails, logging may remain initialized in the current implementation.

---

### `void usrl_shutdown(usrl_ctx_t *ctx)`
Shuts down logging and frees the context.

**Behavior**
- `ctx==NULL` is a no-op.

---

## Publisher API

### `usrl_pub_t *usrl_pub_create(usrl_ctx_t *ctx, const usrl_pub_config_t *config)`
Creates (or attaches to) a publisher for `config->topic` backed by a POSIX SHM object named:

- `"/usrl-%s"` where `%s = topic` [web:15]

**Inputs**
- `ctx` (required)
- `config` (required)
  - `config->topic` (required)
  - `config->slot_count`: default `4096` when `0`
  - `config->slot_size`: default `1024` when `0`
  - `config->ring_type`: `USRL_RING_MWMR` selects MWMR; otherwise SWMR
  - `config->block_on_full`: whether to spin/sleep until room is available
  - `config->rate_limit_hz`: when `>0`, enables publish quota limiter

**SHM sizing**
- Computes: `ring_size = slot_count * slot_size + 1MB`
- Requests: `requested_shm_size = max(ring_size, default_min_bytes)`

**Attach/create semantics**
- Calls `usrl_core_init(shm_path, requested_shm_size, &tcfg, 1)`
  - `< 0`: fails and returns `NULL`
  - `== 1`: “already exists” is treated as normal attach (not an error)

**Mapping detail (important)**
- Discovers *actual* SHM object size with `shm_open + fstat` and maps using that size. [web:9][web:15]  
  Rationale: mapping and later `munmap()` must use consistent lengths; `fstat()` provides authoritative `st_size`. [web:9][web:15]

**Returns**
- Non-NULL `usrl_pub_t*` on success.
- `NULL` on any failure (invalid args, core init failure, size discovery failure, mmap failure, allocation failure).

**Nuances / gotchas**
- Topic is copied into fixed 64-byte storage (`63` chars + NUL), so long names are truncated internally.
- A per-process publisher id (`my_id`) is allocated via atomic fetch-add; it is not persisted across process restarts.
- MWMR publishers may attach concurrently; the “already exists” path is expected.

---

### `int usrl_pub_send(usrl_pub_t *pub, const void *data, uint32_t len)`
Publishes one message.

**Returns**
- `0` on success
- `-1` on failure (including dropped due to throttle or ring full when non-blocking)

**Rate limit path (if enabled)**
- If `usrl_quota_check()` returns throttled:
  - `block_on_full==true`: sleeps for a computed backoff interval (ns → µs ceiling) then proceeds
  - `block_on_full==false`: increments `pub->local_drops` and returns `-1`

**Ring-full handling**
- MWMR: retries on `USRL_RING_FULL` or `USRL_RING_TIMEOUT` while `block_on_full==true`, sleeping `usleep(1)` between retries.
- SWMR: retries on `USRL_RING_FULL` while `block_on_full==true`, sleeping `usleep(1)` between retries.

**Nuances**
- `usleep()` takes microseconds; short sleeps are not precise and may overshoot due to OS scheduling. [web:7][web:10]
- If final publish result is `USRL_RING_FULL`, `pub->local_drops` is incremented.
- Non-blocking mode converts transient pressure into immediate drop (`-1`) rather than backoff.

---

### `void usrl_pub_get_health(usrl_pub_t *pub, usrl_health_t *out)`
Fills `out` with publisher health.

**Behavior**
- If shared `RingHealth` is available:
  - `out->operations = rh->pub_health.total_published`
  - `out->rate_hz    = rh->pub_health.publish_rate_hz`
  - `out->errors     = pub->local_drops`
  - `out->lag        = 0`
  - `out->healthy    = (out->errors == 0)`
- If shared health unavailable:
  - `out` is zeroed and `out->errors = pub->local_drops`

**Nuance**
- `errors` here effectively means “local drops” (throttle drops + ring-full drops), not all possible failure classes.

---

### `void usrl_pub_destroy(usrl_pub_t *pub)`
Unmaps SHM and frees publisher.

**Nuance**
- Uses the stored `map_size` (discovered via `fstat`) for `munmap` so the unmap length matches the mapping length. [web:9][web:15]

---

## Subscriber API

### `usrl_sub_t *usrl_sub_create(usrl_ctx_t *ctx, const char *topic)`
Attaches a subscriber to an existing topic SHM object named `"/usrl-%s"`. [web:15]

**Behavior**
- Discovers SHM size via `shm_open + fstat`; if size is 0 or discovery fails, returns `NULL`. [web:9][web:15]
- Maps using the discovered size (same mapping/unmapping correctness rationale). [web:9][web:15]
- Initializes core subscriber with `usrl_sub_init(&sub->core, base, topic)`.

**Nuances**
- This does not create topics; publisher side (or some creator) must have created and sized the SHM object first. [web:9][web:15]
- Topic is stored in a fixed 64-byte buffer (63 + NUL).

---

### `int usrl_sub_recv(usrl_sub_t *sub, void *buffer, uint32_t max_len)`
Receives the next message.

**Returns**
- `>= 0`: number of bytes copied into `buffer`
- `-11`: no data (`USRL_RING_NO_DATA`)
- `-1`: truncation (`USRL_RING_TRUNC`) or ring error (`USRL_RING_ERROR`) or invalid inputs

**Counters**
- On truncation: `sub->local_skips++`
- On ring error: `sub->local_errors++`
- On success: `sub->local_ops++`

**Nuance**
- Truncation is treated as a “skip” class, not a ring error.

---

### `void usrl_sub_get_health(usrl_sub_t *sub, usrl_health_t *out)`
Fills `out` with subscriber-local health and computes lag for SWMR when descriptor is available.

**Fields**
- `out->operations = sub->local_ops`
- `out->errors = sub->local_skips + sub->local_errors + sub->core.skipped_count`
- `out->rate_hz = 0` (not computed here)
- `out->lag`:
  - If `sub->core.desc` is present (SWMR descriptor available):
    - `w_head = usrl_swmr_total_published(sub->core.desc)`
    - `my_seq = sub->core.last_seq`
    - `lag = max(0, w_head - my_seq)`
  - Else: `lag = 0`
- `out->healthy = (out->lag < 100 && out->errors == 0)`

**Nuances**
- `healthy` policy is strict and hard-coded: any errors fail health; lag must be < 100.
- `errors` includes both facade counters and core `skipped_count`, so it reflects application-level skips too.

---

### `void usrl_sub_destroy(usrl_sub_t *sub)`
Unmaps SHM and frees subscriber.

**Nuance**
- Uses `map_size` discovered via `fstat` for `munmap` correctness. [web:9][web:15]

---

## Practical notes (implementation nuances)

- **Mapping length correctness:** `munmap()` should use the same length passed to `mmap()`, and `fstat()` on the SHM fd is the standard way to determine the shared memory object’s current `st_size` before mapping. [web:9][web:15]
- **usleep granularity:** `usleep()` works in microseconds, but the system may sleep longer; avoid assuming deterministic 1µs pacing in backpressure loops. [web:7][web:10]
- **Name/path limits:** Internal topic buffers are 64 bytes; SHM path buffer is 128 bytes. Plan topic naming accordingly.
- **Return code convention:** Subscriber uses `-11` as “no data” sentinel; consider exposing a named constant in public headers for readability.
