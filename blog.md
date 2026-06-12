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
- `module_pathname` - where the compiled library lives, substituted for `MODULE_PATHNAME` in the SQL script
- `relocatable = false` + `schema = tama` - the extension owns a fixed schema that Postgres creates for it. `pg_` extensions conventionally drop the prefix for the schema, like `pg_cron` and `cron.schedule()`

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

- The `name--version.sql` naming is how upgrades work. Postgres chains `0.1.0--0.2.0` scripts between versions itself.
- Everything the script creates is owned by the extension, `DROP EXTENSION` takes it all with it.
- Extension tables are skipped by `pg_dump` unless marked as user data with `pg_extension_config_dump`, so the pet survives backups.
- `LANGUAGE C` functions point at a symbol in the compiled library.
- The bool primary key with a CHECK means the table can only ever hold one row. One pet per database, enforced by the schema itself.
- The functions run with the caller's privileges, so the script grants access. The pet is communal.

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

The big ideas in this file.

- **The magic block.** `PG_MODULE_MAGIC_EXT` stamps the library with the Postgres version it was built for, and the server refuses to load a mismatch.
- **The V1 calling convention.** Every value crossing the SQL/C boundary is a `Datum`, and the `PG_GETARG_*` / `PG_RETURN_*` macros pack and unpack them.
- **Errors don't return.** `ereport(ERROR)` longjmps out and aborts the transaction. No cleanup code needed, because everything was palloc'd in a memory context the abort destroys wholesale.
- **SPI** is how C runs SQL inside the server, the same machinery PL/pgSQL uses. With `$1` parameters the pet's name is data, not SQL, so Bobby Tables is just a pet.
- **Concurrency is settled by the index.** `ON CONFLICT DO NOTHING` makes the insert itself the authority on whether a pet exists, no racy check-then-insert.
- **Ask the catalogs where you live.** The C resolves the extension's schema at call time instead of hardcoding `tama`, so a renamed schema doesn't break anything.

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

The tick is a placeholder for now, it just logs. The big ideas around it.

- **Workers register at preload.** That's why `shared_preload_libraries` exists and why enabling a worker needs a restart.
- **The postmaster supervises it.** It shows up in `pg_stat_activity` like any backend, gets restarted (or not, `BGW_NEVER_RESTART`) if it dies, and connects to one database like a normal session.
- **The nap is a latch, not a sleep.** It wakes on a timer or instantly on a signal, so shutdown never waits for the tick interval.
- **Signals are flags.** SIGHUP asks for a config reload, SIGTERM ends the worker through the standard `die` handler.
- **GUCs are real settings.** `pg_tamagotchi.tick_interval` gets bounds, units, and a reload policy, change it with a config reload and the worker picks it up on its next nap.

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
