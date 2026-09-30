# Returning memory to the operating system

Two things kept the emulator's resident memory high long after the work that
needed it was done:

- **glibc keeps freed memory.** Each thread allocates from one of glibc's
  arenas, and memory freed there stays reserved for that arena. After a
  database is dropped, its memory is free but still resident, so the process
  settles near the most its busiest threads ever held at once.
- **Old schema versions were removed only by the next schema change.** A
  database keeps the schema versions reads inside its `version_retention_period`
  can need. Versions older than that were removed only when the database's
  schema next changed, so a database migrated one statement at a time and then
  left alone kept every version until it was dropped.

The emulator now frees both in the background, from its memory reclaimer
thread. No request waits for either.

## Returning free heap memory

These ask for free heap memory to be returned to the operating system:

- dropping a database, including each database of a deleted instance;
- a schema change (`UpdateDatabaseDdl`), whether or not it succeeds;
- a sweep that removed schema versions (below).

Ephemeral sessions' copies do not ask: they are made and freed often, and each
is small. Their memory is returned with the next release asked for by
something else.

Every `--heap_release_interval_seconds` (default 10), counted from the end of
its previous release, the reclaimer returns the free heap memory if anything
asked for that since: one release however many requests. With glibc (the
Linux image) that is `malloc_trim(0)`, which returns every free page of every
arena; each release logs one line with its duration, the resident size before
and after, and the heap in use. Elsewhere, such as the native macOS build, a
release does nothing. `0` disables releases.

A request only sets a flag. `malloc_trim` runs on the reclaimer's thread and
holds each arena's lock while it trims that arena, so a request that allocates
from that arena at that moment waits for it to finish.

## Expiring schema versions

Every `--schema_version_gc_interval_seconds` (default 60), counted from the end
of its previous sweep, the reclaimer removes from every database the schema
versions no read can need any more, with the code a schema change uses: it
keeps the database's first schema, the newest version created at or before now
minus the retention period, and every version after it. `0` disables the
sweep; the next schema change of a database still removes its expired
versions, as upstream does.

Anything that uses a schema owns it, so removing a version never frees a
schema in use: a read-only transaction, its cursors and its queries keep the
version they took until they are done.

A read-only transaction reading past the retention period fails, as before,
with `FAILED_PRECONDITION` and the message `Read-only transaction timestamp
<T> has exceeded the maximum timestamp staleness`. Reads at times whose schema
version is still kept are unchanged.

A read-only transaction reading at a time whose schema version a sweep removed
gets that same error from `Read`, `StreamingRead`, `ExecuteSql`,
`ExecuteStreamingSql` and `PartitionQuery`, before the request is resolved
against any schema. That holds even after the database's retention period is
lengthened to cover the time again: lengthening it does not bring back what
was collected, as in Spanner, where "when you extend the retention period, the
system doesn't backfill previous versions of data"
([point-in-time recovery](https://cloud.google.com/spanner/docs/use-pitr)).
Resolving such a request against the older schema that is left would answer
from a schema that was not in effect then: a table not found, or
`INFORMATION_SCHEMA` rows of the wrong time.

Schema versions that a schema change removes keep upstream's behaviour: reads
at their times are resolved against the older schema that is left, as
upstream's conformance tests expect. With
`--schema_version_gc_interval_seconds=0` nothing is swept, so every read
behaves as upstream's.

## What is not freed early

- **Row versions.** Storage removes a cell's versions older than the retention
  period only when that cell is written again. A row written many times and
  then left alone keeps every version, and a deleted row keeps its key and
  deletion markers.
- **Dropped tables and columns.** A dropped table's data is removed by the
  first schema change or read-only read after the retention period; a dropped
  column's data only by the first schema change after it.
