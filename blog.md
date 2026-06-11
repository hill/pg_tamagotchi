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

### Care instructions

```sql
CREATE EXTENSION pg_tamagotchi;

SELECT tama.status();          -- a speckled egg, waiting
SELECT tama.hatch('Blobby');   -- every pet needs a name
SELECT tama.status();          -- check in on them
```

One pet per database. Hatching a second is refused. Neglecting the first is, regrettably, allowed.


## Building Posgres Extensions

<!-- Concise notes, expand into prose later. -->

An extension is a name that ties together three artifacts. `CREATE EXTENSION pg_tamagotchi` finds them by name in Postgres's install directories and wires them into the database.

### pg_tamagotchi.control

```
comment = 'a tamagotchi that lives in your database'
default_version = '0.1.0'
module_pathname = '$libdir/pg_tamagotchi'
relocatable = false
schema = tama
```

The manifest. Lives in `$(pg_config --sharedir)/extension/`, which is how `CREATE EXTENSION pg_tamagotchi` finds the extension at all.

- `comment` - shows up in `\dx`
- `default_version` - which SQL script to run when no version is requested
- `module_pathname` - where the compiled library lives. `$libdir` expands to Postgres's package library directory, and any `MODULE_PATHNAME` string in the SQL script gets substituted with this
- `relocatable = false` + `schema = tama` - the extension owns a fixed schema. Postgres creates `tama` itself and runs the script with `search_path` pinned to it, so the script uses bare names. By convention, `pg_`-prefixed extensions drop the prefix for their schema, the way `pg_cron` gives you `cron.schedule()`. Ours is shortened further because you'll type it a lot.

### pg_tamagotchi--0.1.0.sql

The script that `CREATE EXTENSION` runs.

```sql
\echo Use "CREATE EXTENSION pg_tamagotchi" to load this file. \quit

-- Single-row table holding the pet. The bool primary key with a CHECK
-- makes a second row impossible. Starts empty: an unhatched egg.
CREATE TABLE pet (
    only_one        bool PRIMARY KEY DEFAULT true CHECK (only_one),
    name            text NOT NULL,
    born_at         timestamptz NOT NULL DEFAULT now(),
    hunger          int NOT NULL DEFAULT 20 CHECK (hunger BETWEEN 0 AND 100),
    happiness       int NOT NULL DEFAULT 80 CHECK (happiness BETWEEN 0 AND 100),
    poop            bigint NOT NULL DEFAULT 0,
    stress          int NOT NULL DEFAULT 0,
    cache_hit_ratio float8,
    last_tick_at    timestamptz
);

-- Pet state is user data, not extension furniture: include it in pg_dump
-- so the pet survives backup and restore.
SELECT pg_catalog.pg_extension_config_dump('pet', '');

CREATE FUNCTION hatch(name text) RETURNS text
AS 'MODULE_PATHNAME', 'tama_hatch'
LANGUAGE C VOLATILE;

CREATE FUNCTION status() RETURNS text
AS 'MODULE_PATHNAME', 'tama_status'
LANGUAGE C VOLATILE;
```

A few interesting concepts hide in here.

- The `name--version.sql` naming is load-bearing. Upgrades work by chaining scripts (`pg_tamagotchi--0.1.0--0.2.0.sql`) and Postgres finds the path between any two versions itself.
- Every object the script creates is *owned* by the extension. `DROP EXTENSION` removes them all, `\dx+` lists them, and you can't accidentally drop just the table out from under it.
- Extension-owned tables are skipped by `pg_dump` on the theory that they're furniture, recreatable from the script. Pet state is not furniture. `pg_extension_config_dump('pet', '')` marks its contents as user data so a backup doesn't kill the pet.
- `CREATE FUNCTION ... LANGUAGE C` is the bridge to the compiled half. `'MODULE_PATHNAME', 'tama_hatch'` tells Postgres to dlopen the library named in the control file and look up the symbol `tama_hatch`.
- The single-row-table trick. A `bool` primary key defaulting to `true` with `CHECK (only_one)` can only ever hold one row, since a second insert violates the unique constraint. The schema itself enforces one pet per database.

### src/pg_tamagotchi.c

The compiled half.

```c
#include "postgres.h"

#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC_EXT(.name = "pg_tamagotchi", .version = "0.1.0");

PG_FUNCTION_INFO_V1(tama_hatch);
PG_FUNCTION_INFO_V1(tama_status);

Datum
tama_hatch(PG_FUNCTION_ARGS)
{
	text	   *name;
	char	   *name_cstr;
	StringInfoData buf;
	Oid			argtypes[1] = {TEXTOID};
	Datum		values[1];
	int			ret;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("your egg needs a name to hatch"),
				 errhint("Try SELECT tama.hatch('Blobby');")));

	name = PG_GETARG_TEXT_PP(0);
	name_cstr = text_to_cstring(name);

	if (name_cstr[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a pet cannot be named the empty string"),
				 errhint("Try SELECT tama.hatch('Blobby');")));

	/* The result buffer must outlive SPI's memory, so allocate it first. */
	initStringInfo(&buf);

	SPI_connect();

	ret = SPI_execute("SELECT name FROM tama.pet", true, 1);
	if (ret != SPI_OK_SELECT)
		elog(ERROR, "could not check for an existing pet");
	if (SPI_processed > 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("you already have a pet named %s",
						SPI_getvalue(SPI_tuptable->vals[0],
									 SPI_tuptable->tupdesc, 1)),
				 errhint("One pet per database. Care for the one you have.")));

	values[0] = PointerGetDatum(name);
	ret = SPI_execute_with_args("INSERT INTO tama.pet (name) VALUES ($1)",
								1, argtypes, values, NULL, false, 0);
	if (ret != SPI_OK_INSERT)
		elog(ERROR, "the egg refused to hatch");

	appendStringInfo(&buf,
					 "*crack*\n"
					 " (\\_/)\n"
					 " (o.o)  %s hatched! Take good care of them.\n",
					 name_cstr);

	SPI_finish();

	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}

Datum
tama_status(PG_FUNCTION_ARGS)
{
	StringInfoData buf;
	int			ret;

	initStringInfo(&buf);

	SPI_connect();

	ret = SPI_execute("SELECT name, hunger, happiness, poop, stress"
					  " FROM tama.pet", true, 1);
	if (ret != SPI_OK_SELECT)
		elog(ERROR, "could not look in on the pet");

	if (SPI_processed == 0)
	{
		appendStringInfoString(&buf,
							   "  _____\n"
							   " /     \\   a speckled egg sits here, waiting.\n"
							   " \\_____/   SELECT tama.hatch('a name you like');\n");
	}
	else
	{
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		HeapTuple	tuple = SPI_tuptable->vals[0];
		bool		isnull;
		char	   *name = SPI_getvalue(tuple, tupdesc, 1);
		int32		hunger = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 2, &isnull));
		int32		happiness = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
		int64		poop = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 4, &isnull));
		int32		stress = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 5, &isnull));

		appendStringInfo(&buf,
						 " (\\_/)\n"
						 " (o.o)  %s\n"
						 " (\" \")  hunger %d/100  happiness %d/100"
						 "  stress %d  poop " INT64_FORMAT "\n",
						 name, hunger, happiness, stress, poop);
	}

	SPI_finish();

	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}
```

This file is dense with Postgres internals.

- **The magic block.** `PG_MODULE_MAGIC_EXT` bakes a struct into the library recording the Postgres major version and ABI-relevant build options. Postgres checks it at dlopen time and refuses mismatched binaries. The `_EXT` variant is new in PG 18 and also registers the name and version in `pg_get_loaded_modules()`.
- **The V1 calling convention.** `PG_FUNCTION_INFO_V1(tama_hatch)` plus the fixed signature `Datum tama_hatch(PG_FUNCTION_ARGS)`. Every value crossing the SQL/C boundary is a `Datum`, one pointer-sized blob that might hold an int inline or point to a length-prefixed "varlena" value like `text`. The `PG_GETARG_*` and `PG_RETURN_*` macros do the packing and unpacking.
- **Errors don't return.** `ereport(ERROR, ...)` longjmps out of the function entirely and aborts the transaction. Notice there's no cleanup code on the error paths, nothing closes the SPI connection or frees the buffer. That isn't sloppiness. All that memory belongs to a memory context the transaction abort destroys wholesale. `errcode`, `errmsg`, and `errhint` become the ERROR and HINT lines you see in psql.
- **palloc, never free.** Postgres memory is arena-allocated in a tree of contexts, per-query, per-transaction, and so on. Everything here allocates from the current context and nobody frees anything. The context reset does it.
- **SPI**, the Server Programming Interface, is how C code runs SQL inside the server. It's the same machinery PL/pgSQL uses. `SPI_execute` runs the query, then `SPI_processed` holds the row count, `SPI_tuptable` the results, and `SPI_getbinval` pulls out one column as a Datum.
- **Parameterized queries from C.** `SPI_execute_with_args` with `$1` means the pet's name is never spliced into SQL text, so `hatch('Robert''); DROP TABLE pet;--')` is a pet with a weird name, not little Bobby Tables.
- **Memory context choreography.** `initStringInfo(&buf)` happens before `SPI_connect` on purpose. SPI runs in its own memory arena that `SPI_finish` destroys, but `repalloc` keeps a chunk in the context where it was born, so a buffer allocated before SPI survives appends made during SPI and is still valid after.

### Makefile

```make
MODULE_big = pg_tamagotchi
OBJS = src/pg_tamagotchi.o
EXTENSION = pg_tamagotchi
DATA = pg_tamagotchi--0.1.0.sql
PGFILEDESC = "pg_tamagotchi - a tamagotchi that lives in your database"
REGRESS = basic

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
```

A handful of declarations handed to PGXS, the build system that ships inside Postgres.

- `MODULE_big` + `OBJS` - link these objects into one shared library
- `EXTENSION` - the extension name
- `DATA` - SQL scripts to install
- `REGRESS` - pg_regress test names
- `include $(PGXS)` - pull in Postgres's makefile, which knows the server's exact compiler flags and provides `make install` / `make installcheck`

`make install` copies the control file and SQL script into `sharedir/extension/` and the library into `pkglibdir`. That's the whole trick, `CREATE EXTENSION` is a filesystem lookup followed by running a SQL script.