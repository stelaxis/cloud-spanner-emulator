# CreateDatabase schema cache

Creating a database from a list of DDL statements takes time quadratic in the
number of statements: each statement copies and validates the whole schema
built so far. Tests that create a database per test module, or per test, create
the same schema over and over. The emulator therefore remembers the schemas of
successful `CreateDatabase` requests. A request equal to a remembered one gets
a copy of that schema, without processing its statements.

## When a request is served from the cache

Two requests are equal when they have the same `extra_statements` (the exact
text, in order), dialect and `proto_descriptors`, and are made under the same
emulator feature flags. The database's name is not part of the key: DDL
processing does not depend on it. Command-line flags are fixed for the life of
the process, and so is the cache.

Only schemas of successful creates are remembered. A request that fails always
processes its statements, so it fails exactly as it would without the cache.

## What a database created from the cache gets

A copy that shares nothing with the remembered schema, or with the database it
came from:

* every schema object is cloned;
* column, view and model types are rebuilt in the new database's type factory,
  and the proto bundle is rebuilt from its descriptors;
* sequences get new IDs. The emulator keeps a sequence's state by ID, so a new
  database's sequences, and identity columns, start where they would in a
  database created from DDL, and no two databases draw from the same state;
* its table and column ID counters, and its PostgreSQL OID counter, continue
  from where creating the schema from DDL leaves them, so later schema changes
  assign the same IDs as they would on a database created from DDL;
* its change streams are created at the time of the copy, and get their own
  initial partitions.

A schema the copy does not support is created from DDL every time: one with a
function whose signature has an array, struct, proto or enum type, or default
arguments.

## Configuration

`--schema_create_cache_size=N` (on `emulator_main` and `gateway_main`) is the
number of schemas remembered, least recently used first out. The default is 8.
`0` disables the cache.

The cache is on by default because it changes nothing a client can observe: a
database created from the cache and one created from DDL return the same
`GetDatabaseDdl` and `INFORMATION_SCHEMA` contents, behave the same under later
schema changes, and fail the same way. Its cost is one schema copy per
remembered schema, plus one copy the first time each request is seen.

`backend/database/schema_create_cache_test.cc` and
`tests/conformance/cases/schema_create_cache.cc` check these claims, the latter
over every schema change test case and conformance schema, in both dialects.
