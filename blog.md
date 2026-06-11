# pg_tamagotchi: A tamagotchi that lives in your postgres instance.

[Postgres](https://www.tigerdata.com/blog/its-2026-just-use-postgres) [can](https://postgresforeverything.com/) [do](https://www.amazingcto.com/postgres-for-everything/) [everything](https://www.justfuckingusepostgres.com/)!

You don't need redis, elasticsearch, pinecone, arcgis or a $5000/month bill!

- Full text search = `pg_textsearch`
- Vector database = `pg_vector`
- Cache store = `UNLOGGED` tables
- NoSQL/documnents = `JSONB`
- Geospatial Database = `postgis`
- Queues = `pgmq`
- Cronjobs = `pg_cron`
- REST API = `postgREST`
- GraphQL = `postGraphile`
...

I love Postgres! But you know what's missing from this list? Tamagotchi implemented as a postgres extension. 

Therefore, introducing...

## pg_tamagotchi

pg_tamagotchi is your pet in postgres. You hatch it, name it, feed it, clean up it's shit and love and care for it. Furthermore, it integrates with your database statistics.


## Building Posgres Extensions

<!-- Concise notes, expand into prose later. -->

An extension is a name that ties together three artifacts. `CREATE EXTENSION pg_tamagotchi` finds them by name in Postgres's install directories and wires them into the database.

### pg_tamagotchi.control

The manifest. Lives in `$(pg_config --sharedir)/extension/`. Ours:

- `comment` - shows up in `\dx`
- `default_version = '0.1.0'` - which SQL script to run when no version is requested
- `module_pathname = '$libdir/pg_tamagotchi'` - where the compiled library lives. `$libdir` expands to Postgres's package library directory, and any `MODULE_PATHNAME` string in the SQL script gets substituted with this
- `relocatable = false` + `schema = tama` - the extension owns a fixed schema. Postgres creates `tama` itself and runs the script with `search_path` pinned to it, so the script uses bare names

### pg_tamagotchi--0.1.0.sql

The install script, literally executed by `CREATE EXTENSION`. The `name--version.sql` naming is load-bearing, upgrades work by chaining scripts (`pg_tamagotchi--0.1.0--0.2.0.sql`) and Postgres finds the path between versions itself. Every object the script creates is owned by the extension, `DROP EXTENSION` removes them all.

Ours creates the single-row `pet` table (bool primary key with `CHECK (only_one)` makes a second row impossible), seeds it, and declares the SQL face of the C function:

```sql
CREATE FUNCTION status() RETURNS text
AS 'MODULE_PATHNAME', 'tama_status'
LANGUAGE C VOLATILE;
```

One wrinkle: extension-owned tables are skipped by pg_dump (they're "furniture", recreatable from the script). `SELECT pg_extension_config_dump('pet', '')` marks the pet's row as user data so a backup doesn't kill it.

### src/pg_tamagotchi.c

The compiled half. Two pieces of ceremony:

- `PG_MODULE_MAGIC` - a struct baked into the library recording the Postgres version and ABI-relevant build options. Postgres checks it at load time and refuses mismatched binaries. PG 18 adds `PG_MODULE_MAGIC_EXT(.name, .version)` which also registers us in `pg_get_loaded_modules()`
- `PG_FUNCTION_INFO_V1(tama_status)` plus the fixed signature `Datum tama_status(PG_FUNCTION_ARGS)` - the "version 1 calling convention". Every value crossing the SQL/C boundary is a `Datum`, and macros like `PG_RETURN_TEXT_P` do the packing

### Makefile

Nine lines of declarations handed to PGXS, the build system that ships inside Postgres:

- `MODULE_big` + `OBJS` - link these objects into one shared library
- `EXTENSION` - the extension name
- `DATA` - SQL scripts to install
- `REGRESS` - pg_regress test names
- `include $(PGXS)` - pull in Postgres's makefile, which knows the server's exact compiler flags and provides `make install` / `make installcheck`

`make install` copies the control file and SQL script into `sharedir/extension/` and the library into `pkglibdir`. That's the whole trick, `CREATE EXTENSION` is a filesystem lookup followed by running a SQL script.