# Ephemeral sessions

A test that shares a database with other tests must clean up after itself, or
see their rows. An ephemeral session gives it a private copy of the database
instead: the copy starts with the database's schema and data, takes the test's
writes, and disappears with the session.

## Creating one

Create a session with the label `emulator-ephemeral` set to `"true"`, through
`CreateSession` or `BatchCreateSessions`, on an existing database (the base).
Any other value of the label, or no label, creates an ordinary session, exactly
as upstream does. The label is returned with the session like any other.

Every session-scoped call on an ephemeral session (reads, queries, DML,
transactions, partitioned DML, `BatchWrite`) uses its copy. Nothing else can
reach the copy: each ephemeral session has its own, so two ephemeral sessions
of one base never see each other's writes, and neither sees the base's later
ones. `ListSessions` of the base lists its ephemeral sessions with the others.

## What the copy holds

The emulator picks one timestamp T while creating the session, waits for every
commit with a timestamp at or before T to finish, and copies, as of T:

* the base's schema, with the base's database name, so `INFORMATION_SCHEMA` and
  error messages read as they do on the base;
* every row committed at or before T, in tables, index data and change stream
  tables. A commit is in the copy whole or not at all;
* the counters of sequences and identity columns, so the copy never draws a
  value that a copied row holds. The copy's and the base's counters are
  independent from then on. Counters are not versioned, and a transaction
  draws from one before it commits, so no timestamp's rows match a counter
  exactly: the emulator reads them after T, so they are at least what the
  rows at T drew. A value drawn by a transaction that commits after T is
  skipped by the copy, as a value drawn by a transaction that rolls back is
  skipped by the base.

Proto and enum values take the copy's own types, so the copy stays readable
when the base drops its proto bundle.

The copy's table, column and PostgreSQL OID counters continue from the base's,
so a schema change applied to both assigns the same IDs.

The copy holds one version of each row, at T. Its commits have timestamps
after T. Reads of the copy at a timestamp before T see no rows, where the base
would show its history; bounded-staleness reads never pick a timestamp before
T.

## Schema changes

Schema changes are database-wide calls, not session-scoped, so they cannot
target a copy. `UpdateDatabaseDdl` and `GetDatabaseDdl` on the base's name act
on the base only. Copies keep the schema they were made with; sessions created
afterwards get copies with the new schema. A schema change of the base waits
while a copy of it is being made.

## When the copy goes away

* `DeleteSession` deletes the session and frees its copy.
* An ephemeral session idle for `--ephemeral_session_idle_timeout_seconds`
  (default 120) expires; any call on the session restarts the idle time.
  Upstream expires sessions only when they are looked up, so the copies of a
  test run that was killed would stay. A background thread therefore deletes
  every expired ephemeral session every 10 seconds, whether or not requests
  arrive. Ordinary sessions keep upstream's lazy expiry after an hour idle (28
  days for multiplexed ones).
* `DropDatabase` of the base deletes its ephemeral sessions and their copies.
  A database created again under the same name does not bring them back: they
  are tied to the dropped database object, not to its name.

A copy is destroyed outside the session manager's lock, so the rest of the
emulator keeps serving sessions while it is freed.

## Limits

* At most `--max_ephemeral_sessions` (default 256) copies exist at once,
  including those being created. A copy counts until it is destroyed: a
  request in flight on a deleted or expired session keeps it, and its place,
  until the request ends. One more fails with `RESOURCE_EXHAUSTED`; a
  `BatchCreateSessions` that does not fit fails whole.
  Copies do not count towards `--override_max_databases_per_instance`, and
  ordinary sessions are not limited.
* Multiplexed ephemeral sessions are refused with `INVALID_ARGUMENT`: the
  emulator keeps multiplexed-session transactions by database name, which every
  copy of a base shares.
* A schema the emulator cannot copy fails `CreateSession` with `UNIMPLEMENTED`:
  one with a function whose signature has an array, struct, proto or enum type,
  or default arguments (the same schemas the
  [schema cache](schema-create-cache.md) cannot copy). The emulator never falls
  back to replaying DDL, since that would not reproduce the base's data or
  counters.
* Everything is in memory: each copy costs about as much memory as the base's
  schema and data.

Both flags are accepted by `emulator_main` and `gateway_main`. The emulator
refuses to start with a timeout that is not positive or a negative limit;
`--max_ephemeral_sessions=0` disables ephemeral sessions.

## Tests

`backend/database/ephemeral_copy_test.cc` covers the copy itself, including a
commit that is still flushing when the copy picks T and one that commits after
T but before the copy reads the rows. `frontend/handlers/ephemeral_sessions_test.cc`
covers the sessions through the gRPC API, with the controllable server clock
for expiry.
