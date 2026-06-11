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

-- The pet is communal. The functions run with the caller's privileges,
-- so everyone needs real access to the schema and the table.
GRANT USAGE ON SCHEMA @extschema@ TO PUBLIC;
GRANT SELECT, INSERT, UPDATE ON pet TO PUBLIC;
```

A few interesting concepts hide in here.

- The `name--version.sql` naming is load-bearing. Upgrades work by chaining scripts (`pg_tamagotchi--0.1.0--0.2.0.sql`) and Postgres finds the path between any two versions itself.
- Every object the script creates is *owned* by the extension. `DROP EXTENSION` removes them all, `\dx+` lists them, and you can't accidentally drop just the table out from under it.
- Extension-owned tables are skipped by `pg_dump` on the theory that they're furniture, recreatable from the script. Pet state is not furniture. `pg_extension_config_dump('pet', '')` marks its contents as user data so a backup doesn't kill the pet.
- `CREATE FUNCTION ... LANGUAGE C` is the bridge to the compiled half. `'MODULE_PATHNAME', 'tama_hatch'` tells Postgres to dlopen the library named in the control file and look up the symbol `tama_hatch`.
- The single-row-table trick. A `bool` primary key defaulting to `true` with `CHECK (only_one)` can only ever hold one row, since a second insert violates the unique constraint. The schema itself enforces one pet per database.
- `LANGUAGE C` functions are not `SECURITY DEFINER` by default, they run with the caller's privileges. Without the `GRANT`s, any role other than the installer would get into the function and then hit a raw permission error on the schema. `@extschema@` is substituted with the extension's schema when the script runs.

### src/pg_tamagotchi.c

The compiled half.

```c
#include "postgres.h"

#include "commands/extension.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "pg_tamagotchi.h"

PG_MODULE_MAGIC_EXT(.name = "pg_tamagotchi", .version = "0.1.0");

PG_FUNCTION_INFO_V1(tama_hatch);
PG_FUNCTION_INFO_V1(tama_status);

int			tama_tick_interval = 10;
char	   *tama_database = NULL;

void
_PG_init(void)
{
	BackgroundWorker worker;

	/* GUCs are defined whenever the library loads, by any backend */
	DefineCustomIntVariable("pg_tamagotchi.tick_interval",
							"Seconds between pet ticks.",
							NULL,
							&tama_tick_interval,
							10, 1, 3600,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	DefineCustomStringVariable("pg_tamagotchi.database",
							   "Database the pet lives in.",
							   NULL,
							   &tama_database,
							   "postgres",
							   PGC_POSTMASTER,
							   0,
							   NULL, NULL, NULL);

	MarkGUCPrefixReserved("pg_tamagotchi");

	/* Workers can only be registered while the postmaster is preloading */
	if (!process_shared_preload_libraries_in_progress)
		return;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;

	/*
	 * Never auto-restart. If the configured database doesn't exist the
	 * worker dies FATAL at connect; a restart interval would turn that
	 * one log line into an infinite crash loop the operator can only
	 * stop with a full cluster restart.
	 */
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(worker.bgw_library_name, sizeof(worker.bgw_library_name),
			 "pg_tamagotchi");
	snprintf(worker.bgw_function_name, sizeof(worker.bgw_function_name),
			 "tama_worker_main");
	snprintf(worker.bgw_name, sizeof(worker.bgw_name), "pg_tamagotchi worker");
	snprintf(worker.bgw_type, sizeof(worker.bgw_type), "pg_tamagotchi");
	RegisterBackgroundWorker(&worker);
}

/*
 * The pet table, qualified with whatever schema the extension actually
 * lives in. Hardcoding "tama.pet" would break if the schema is renamed,
 * and worse, would follow an impostor schema recreated under that name.
 */
static char *
tama_pet_relname(void)
{
	Oid			extoid = get_extension_oid("pg_tamagotchi", false);
	char	   *nspname = get_namespace_name(get_extension_schema(extoid));

	return psprintf("%s.pet", quote_identifier(nspname));
}

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

	/*
	 * The insert itself is the authority on whether a pet exists. A
	 * separate check-then-insert would race against concurrent hatches
	 * and read a stale snapshot when called twice in one statement.
	 */
	values[0] = PointerGetDatum(name);
	ret = SPI_execute_with_args(
		psprintf("INSERT INTO %s (name) VALUES ($1) ON CONFLICT DO NOTHING",
				 tama_pet_relname()),
		1, argtypes, values, NULL, false, 0);
	if (ret != SPI_OK_INSERT)
		elog(ERROR, "the egg refused to hatch");

	if (SPI_processed == 0)
	{
		ret = SPI_execute(psprintf("SELECT name FROM %s", tama_pet_relname()),
						  false, 1);
		if (ret != SPI_OK_SELECT)
			elog(ERROR, "could not look in on the pet");
		if (SPI_processed > 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNIQUE_VIOLATION),
					 errmsg("you already have a pet named %s",
							SPI_getvalue(SPI_tuptable->vals[0],
										 SPI_tuptable->tupdesc, 1)),
					 errhint("One pet per database. Care for the one you have.")));
		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("you already have a pet"),
				 errhint("One pet per database. Care for the one you have.")));
	}

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

	ret = SPI_execute(psprintf("SELECT name, hunger, happiness, poop, stress"
							   " FROM %s", tama_pet_relname()),
					  false, 1);
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
OBJS = src/pg_tamagotchi.o src/worker.o
EXTENSION = pg_tamagotchi
DATA = pg_tamagotchi--0.1.0.sql
PGFILEDESC = "pg_tamagotchi - a tamagotchi that lives in your database"
# Only tests that pass on any cluster. The worker test needs
# shared_preload_libraries, so `just test` opts in with
# REGRESS="basic worker" against the dev cluster.
REGRESS = basic

# Apple clang doesn't know the syslog format archetype used by PG's
# printf attributes; without this every build emits 31 copies of the
# same harmless warning.
PG_CFLAGS = -Wno-ignored-attributes

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

## The background worker

A real tamagotchi gets hungry while you're not looking. SQL functions only run when called, so the aging logic needs a process of its own. Postgres has exactly this concept, the background worker. The postmaster starts it, supervises it, and restarts it if it crashes. Autovacuum and logical replication run on the same machinery, and extensions get to use it too.

The worker lives in `src/worker.c`. Registration happens in `_PG_init` above.

```c
#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/wait_event.h"

#include "pg_tamagotchi.h"

PGDLLEXPORT void tama_worker_main(Datum main_arg);

static void
tama_tick(void)
{
	elog(LOG, "pg_tamagotchi: tick");
}

void
tama_worker_main(Datum main_arg)
{
	uint32		wait_event_info;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(tama_database, NULL, 0);

	wait_event_info = WaitEventExtensionNew("PgTamagotchiMain");

	elog(LOG, "pg_tamagotchi: worker started, the pet lives in database \"%s\"",
		 tama_database);

	for (;;)
	{
		tama_tick();

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 tama_tick_interval * 1000L,
						 wait_event_info);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
	}
}
```

The tick is a placeholder for now, it just logs. The interesting parts are around it.

- **Registration only works at preload.** Workers must be registered while the postmaster processes `shared_preload_libraries`, so the library has to be in that list and changing it needs a full restart. A replaced library file on disk also only takes effect at restart, because every backend inherits the postmaster's already-loaded image via fork.
- **The registration struct does a lot.** `bgw_type` becomes `backend_type` in `pg_stat_activity`. `BGWORKER_BACKEND_DATABASE_CONNECTION` lets it connect to one database like a real backend, which is what `pg_tamagotchi.database` points at. `bgw_restart_time` is a trap, a restart interval sounds like resilience, but if the configured database doesn't exist the worker dies FATAL at connect and the postmaster would relaunch it into the same FATAL forever. `BGW_NEVER_RESTART` keeps a misconfiguration at one log line, and it's what the in-tree `worker_spi` example uses too.
- **The nap is a latch, not a sleep.** `WaitLatch` wakes on a timeout like sleep would, but also instantly when something sets the latch, like the signal handlers do. Shutdown is immediate instead of "up to tick_interval later". `WL_EXIT_ON_PM_DEATH` kills the worker if the postmaster vanishes, so it can never outlive the server.
- **Named wait events.** `WaitEventExtensionNew("PgTamagotchiMain")` registers the name monitoring tools see while the worker naps. Our pet shows up in `pg_stat_activity` as `Extension / PgTamagotchiMain`, right next to the built-in wait events.
- **Signals are flags, mostly.** SIGHUP sets `ConfigReloadPending` and the loop reloads config when it notices. SIGTERM is wired to `die`, the standard backend handler, which raises FATAL at the next `CHECK_FOR_INTERRUPTS()`. The shutdown log line reads `FATAL: terminating background worker "pg_tamagotchi" due to administrator command`, which looks alarming and is the system working as designed.
- **GUCs are real settings.** `DefineCustomIntVariable` gives `pg_tamagotchi.tick_interval` bounds, units (you can write `'30s'` or `'5min'`), and a change policy. `PGC_SIGHUP` means a config reload is enough, and the worker picks the new value up on its next nap. The database setting is `PGC_POSTMASTER` because the worker connects exactly once at startup. `MarkGUCPrefixReserved` turns misspelled settings into warnings instead of silent no-ops.

Proof of life.

```
$ just log
LOG:  pg_tamagotchi: worker started, the pet lives in database "postgres"
LOG:  pg_tamagotchi: tick
LOG:  pg_tamagotchi: tick

$ psql -c "SELECT pid, backend_type, datname, wait_event FROM pg_stat_activity
           WHERE backend_type = 'pg_tamagotchi';"
  pid  | backend_type  | datname  |    wait_event
-------+---------------+----------+------------------
 64708 | pg_tamagotchi | postgres | PgTamagotchiMain
```

## Sharp edges an adversarial review caught

<!-- notes; a fleet of review agents went over the C and confirmed these, each is a concept worth a paragraph -->

- Check-then-insert is a race. The original `hatch()` did SELECT then INSERT, so two concurrent hatches both passed the check and the loser got a raw `pet_pkey` violation instead of the friendly error. Worse, the check ran with `read_only=true`, which reuses the statement's snapshot, so `SELECT tama.hatch(n) FROM (VALUES ('a'),('b')) v(n)` slipped past it too. `INSERT ... ON CONFLICT DO NOTHING` made the insert itself the authority.
- A worker restart interval sounds like resilience and is actually a crash loop when the failure is deterministic, like a missing database. `BGW_NEVER_RESTART`.
- C functions run with the caller's privileges. Defaults gave other roles EXECUTE on the functions but no access to the schema behind them, a confusing half-open door. The script now grants real access on purpose.
- Hardcoding `tama.pet` in the C breaks if someone renames the schema, and the extension can't stop them, the schema isn't an extension member even though `CREATE EXTENSION` made it. The C now resolves the schema from the catalogs (`get_extension_oid`, `get_extension_schema`) at call time.