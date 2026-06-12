# pg_tamagotchi: A pet for your postgres db

[Postgres](https://www.tigerdata.com/blog/its-2026-just-use-postgres) [can](https://postgresforeverything.com/) [do](https://www.amazingcto.com/postgres-for-everything/) [everything](https://www.justfuckingusepostgres.com/)!

You don't need redis, elasticsearch, pinecone, arcgis or a $5000/month bill!

- Full text search = `pg_textsearch`
- Vector database = `pg_vector`
- Cache store = `UNLOGGED` tables
- NoSQL/documents = `JSONB`
- Geospatial Database = `postgis`
- Queues = `pgmq`
- Cronjobs = `pg_cron`
- REST API = `postgREST`
- GraphQL = `postGraphile`
...

I love Postgres! But you know what's missing from this list? Tamagotchi implemented as a postgres extension.

Therefore, introducing...

## pg_tamagotchi

pg_tamagotchi is your pet in postgres. You hatch it, name it, feed it, clean up its shit and love and care for it.

A pet's happiness depends on its environment, so we also monitor postgres statistics and your pet will respond.

### Care instructions

```sql
CREATE EXTENSION pg_tamagotchi;

SELECT tama.status();          -- a speckled egg, waiting
SELECT tama.hatch('Ludo');     -- every pet needs a name
-- (skip the argument and a pronounceable name is invented)
SELECT tama.feed('apple');     -- snacks help
SELECT tama.talk('hello?');    -- the pet talks back
SELECT tama.status();          -- check in on them
```

That's the whole setup. The pet ages whenever someone checks in, and conversations are stored in the database with the pet.

## What is a Postgres Extension?

For a C-backed extension like this one, an extension is a name that ties together three artifacts.

1. A **control file** declaring the extension exists.
2. A **SQL script** that creates the schema, tables, and function signatures.
3. A **shared library** holding the compiled C those function signatures point at.

`CREATE EXTENSION pg_tamagotchi` finds them by name in Postgres's install directories and wires them into the database.

_"Install directories"_ are the fixed locations on disk that pg will look for extension files. `pg_config --sharedir` will print the path for the _sharedir_, where the control file and SQL scripts live. `pg_config --pkglibdir` is the directory where the compiled shared libraries, e.g. `.so` or `.dylib`, live.

### pg_tamagotchi.control

```
comment = 'a tamagotchi that lives in your database'
default_version = '0.1.0'
module_pathname = '$libdir/pg_tamagotchi'
relocatable = false
schema = tama
```

This is the extension manifest. It lives in `$(pg_config --sharedir)/extension/`, which is how `CREATE EXTENSION pg_tamagotchi` finds the extension at all.

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
    last_tick_at    timestamptz NOT NULL DEFAULT clock_timestamp()
);

-- Conversation history with the pet. This is user data too.
CREATE TABLE message (
    said_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    speaker text NOT NULL CHECK (speaker IN ('you', 'pet')),
    body text NOT NULL
);

-- Pet state is user data, not extension furniture: include it in pg_dump
-- so the pet survives backup and restore.
SELECT pg_catalog.pg_extension_config_dump('pet', '');
SELECT pg_catalog.pg_extension_config_dump('message', '');

-- With no name, a pronounceable one is invented.
CREATE FUNCTION hatch(name text DEFAULT NULL) RETURNS text
AS 'MODULE_PATHNAME', 'tama_hatch'
LANGUAGE C VOLATILE;

CREATE FUNCTION feed(food text DEFAULT NULL) RETURNS text
AS 'MODULE_PATHNAME', 'tama_feed'
LANGUAGE C VOLATILE;

CREATE FUNCTION talk(message text DEFAULT NULL) RETURNS text
AS 'MODULE_PATHNAME', 'tama_talk'
LANGUAGE C VOLATILE;

CREATE FUNCTION status() RETURNS text
AS 'MODULE_PATHNAME', 'tama_status'
LANGUAGE C VOLATILE;

-- The pet is communal. The functions run with the caller's privileges,
-- so everyone needs real access to the schema and the table.
GRANT USAGE ON SCHEMA @extschema@ TO PUBLIC;
GRANT SELECT, INSERT, UPDATE ON pet TO PUBLIC;
GRANT SELECT, INSERT ON message TO PUBLIC;
```

- The `name--version.sql` naming is how upgrades work. Postgres chains `0.1.0--0.2.0` scripts between versions itself.
- Everything the script creates is owned by the extension, and `DROP EXTENSION` takes it all with it.
- `pg_dump` emits `CREATE EXTENSION` instead of dumping extension-owned objects directly. `pg_extension_config_dump` marks the pet and message tables as user data, so both survive backups.
- `LANGUAGE C` functions point at a symbol in the compiled library.
- The bool primary key with a CHECK means the table can only ever hold one row. One pet per database, enforced by the schema itself.
- `last_tick_at` is the pet's clock. Since it is ordinary table state, it survives backup and restore with the rest of the pet.
- `message` is the conversation log. `tama.talk()` writes one row for you and one row for the pet's reply.
- The functions run with the caller's privileges, so the script grants access. The pet is communal.

### src/pg_tamagotchi.c

This is the C source code that function signatures link to.

```c
#include "postgres.h"

#include <string.h>

#include "commands/extension.h"
#include "common/pg_prng.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC_EXT(.name = "pg_tamagotchi", .version = "0.1.0");

PG_FUNCTION_INFO_V1(tama_hatch);
PG_FUNCTION_INFO_V1(tama_feed);
PG_FUNCTION_INFO_V1(tama_talk);
PG_FUNCTION_INFO_V1(tama_status);

#define TAMA_TICK_SECONDS 10
```

A small helper resolves the extension's actual schema at runtime, rather than hardcoding `tama.pet` or `tama.message`.

```c
/*
 * A table qualified with whatever schema the extension actually lives in.
 * Hardcoding "tama.pet" would break if the schema is renamed, and worse,
 * would follow an impostor schema recreated under that name.
 */
static char *tama_relname(const char *relname) {
  Oid extoid = get_extension_oid("pg_tamagotchi", false);
  char *nspname = get_namespace_name(get_extension_schema(extoid));

  return psprintf("%s.%s", quote_identifier(nspname), quote_identifier(relname));
}

static char *tama_pet_relname(void) {
  return tama_relname("pet");
}

static char *tama_message_relname(void) {
  return tama_relname("message");
}
```

The tick is the pet's heartbeat. It runs when someone checks `status()`, catches up by however many ten-second intervals have elapsed, and samples a few Postgres statistics.

- dead tuples = poop, and `VACUUM` is the pooper scooper
- cache hit ratio = environment
- sessions sitting idle in transaction = stress
- hunger climbs one point per tick

```c
/*
 * One beat of the pet's life. Time is caught up lazily when someone checks
 * on the pet, so CREATE EXTENSION is enough to make it age.
 */
static void tama_tick(void) {
  char *relname = tama_pet_relname();
  int ret;

  ret = SPI_execute(psprintf(
    "WITH tick AS ("
    "  SELECT only_one,"
    "         greatest(0, floor(extract(epoch FROM"
    "           (clock_timestamp() - last_tick_at)) / %d))::int AS elapsed"
    "  FROM %s"
    "  FOR UPDATE"
    ") "
    "UPDATE %s p SET"
    "  hunger = least(p.hunger + tick.elapsed, 100),"
    "  poop = (SELECT coalesce(sum(n_dead_tup), 0)"
    "          FROM pg_stat_user_tables),"
    "  cache_hit_ratio = (SELECT blks_hit::float8"
    "                            / nullif(blks_hit + blks_read, 0)"
    "                     FROM pg_stat_database"
    "                     WHERE datname = current_database()),"
    "  stress = (SELECT count(*) FROM pg_stat_activity"
    "            WHERE state = 'idle in transaction'),"
    "  last_tick_at = CASE WHEN tick.elapsed > 0"
    "    THEN p.last_tick_at + tick.elapsed * (%d * interval '1 second')"
    "    ELSE p.last_tick_at"
    "  END"
    " FROM tick"
    " WHERE p.only_one = tick.only_one",
    TAMA_TICK_SECONDS, relname, relname, TAMA_TICK_SECONDS), false, 0);

  if (ret != SPI_OK_UPDATE) {
    elog(ERROR, "pg_tamagotchi: tick failed");
  }
}
```

The big ideas in the tick.

- **The clock is data.** The pet does not need a separate process. `last_tick_at` records the last time its state advanced, and the next `status()` applies the missing time.
- **The row lock is the timer guard.** `FOR UPDATE` locks the one pet row while elapsed time is calculated, so two people checking in at once do not both charge the same missing ticks.
- **The stats are just tables.** `pg_stat_user_tables`, `pg_stat_database`, and `pg_stat_activity` are queryable like anything else, so one UPDATE harvests them all.
- **Dead tuples are old row versions.** Updates and deletes leave them behind until vacuum cleans them up. That maps cleanly to pet poop.
- **Idle transactions are stressful.** A session sitting idle in transaction can hold old snapshots open and interfere with cleanup, so the pet notices.
- **Use wall clock time.** `clock_timestamp()` is the actual time right now. That's what a pet wants.

`tama_hatch` is the C side of `SELECT tama.hatch('Ludo')`. It picks a name, attempts a single insert, and renders some ASCII.

```c
/* SELECT tama.hatch(name). Creates the pet, refuses if one already exists. */
Datum tama_hatch(PG_FUNCTION_ARGS) {
  char *name_cstr;
  StringInfoData buf;
  Oid argtypes[1] = {TEXTOID};
  Datum values[1];
  int ret;

  /* A nameless egg gets a pronounceable name minted for it */
  if (PG_ARGISNULL(0)) {
    name_cstr = tama_random_name();
  } else {
    name_cstr = text_to_cstring(PG_GETARG_TEXT_PP(0));
  }

  if (name_cstr[0] == '\0') {
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("a pet cannot be named the empty string"),
             errhint("Try SELECT tama.hatch('Blobby'); or let "
                     "tama.hatch() pick a name.")));
  }

  /* The result buffer must outlive SPI's memory, so allocate it first. */
  initStringInfo(&buf);

  SPI_connect();

  /*
   * The insert itself is the authority on whether a pet exists. A
   * separate check-then-insert would race against concurrent hatches
   * and read a stale snapshot when called twice in one statement.
   */
  values[0] = PointerGetDatum(cstring_to_text(name_cstr));
  ret = SPI_execute_with_args(
    psprintf("INSERT INTO %s (name) VALUES ($1) ON CONFLICT DO NOTHING",
             tama_pet_relname()),
    1, argtypes, values, NULL, false, 0);
  if (ret != SPI_OK_INSERT) {
    elog(ERROR, "the egg refused to hatch");
  }

  if (SPI_processed == 0) {
    ret = SPI_execute(psprintf("SELECT name FROM %s", tama_pet_relname()),
                      false, 1);
    if (ret != SPI_OK_SELECT) {
      elog(ERROR, "could not look in on the pet");
    }
    if (SPI_processed > 0) {
      ereport(ERROR,
              (errcode(ERRCODE_UNIQUE_VIOLATION),
               errmsg("you already have a pet named %s",
                      SPI_getvalue(SPI_tuptable->vals[0],
                                   SPI_tuptable->tupdesc, 1)),
               errhint("One pet per database. Care for the one you have.")));
    }
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
```

`tama_feed` is another small state transition. It catches the pet up first, lowers hunger, nudges happiness up, and returns the new vitals.

```c
ret = SPI_execute(psprintf(
  "UPDATE %s SET"
  "  hunger = greatest(hunger - 25, 0),"
  "  happiness = least(happiness + CASE WHEN hunger > 0 THEN 5 ELSE 1 END, 100)"
  " RETURNING name, hunger, happiness",
  tama_pet_relname()), false, 1);
```

So feeding from SQL looks like this.

```
postgres=# SELECT tama.feed('apple');
                         feed
------------------------------------------------------
 Ludo munches the apple. hunger 0/100  happiness 85/100
```

`tama_talk` reads the current pet state, chooses a reply, and stores the exchange in `tama.message`.

```c
if (message_cstr != NULL) {
  tama_log_message("you", message_cstr);
}
tama_log_message("pet", reply);
```

The reply is deliberately local and deterministic. No network call, no model, no surprise dependency. The pet talks back from the state already in Postgres.

```
postgres=# SELECT tama.talk('how are you?');
                                  talk
------------------------------------------------------------------------
 you: how are you?
 Ludo says: That is a very good question for a tiny database pet.

postgres=# SELECT speaker, body FROM tama.message ORDER BY said_at;
 speaker |                                body
---------+--------------------------------------------------------------------
 you     | how are you?
 pet     | Ludo says: That is a very good question for a tiny database pet.
```

`tama_status` is the read counterpart. It advances the pet, selects the single row, and renders either an egg or the pet's vitals.

```c
/* SELECT tama.status(). Renders the unhatched egg or the pet's vitals. */
Datum tama_status(PG_FUNCTION_ARGS) {
  StringInfoData buf;
  int ret;

  initStringInfo(&buf);

  SPI_connect();

  tama_tick();

  ret = SPI_execute(psprintf("SELECT name, hunger, happiness, poop, stress"
                             " FROM %s", tama_pet_relname()),
                    false, 1);
  if (ret != SPI_OK_SELECT) {
    elog(ERROR, "could not look in on the pet");
  }

  if (SPI_processed == 0) {
    appendStringInfoString(&buf,
                           "  _____\n"
                           " /     \\   a speckled egg sits here, waiting.\n"
                           " \\_____/   SELECT tama.hatch('a name you like');\n");
  } else {
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    HeapTuple tuple = SPI_tuptable->vals[0];
    bool isnull;
    char *name = SPI_getvalue(tuple, tupdesc, 1);
    int32 hunger = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 2, &isnull));
    int32 happiness = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 3, &isnull));
    int64 poop = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 4, &isnull));
    int32 stress = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 5, &isnull));

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

The big ideas in the C.

- **The magic block.** `PG_MODULE_MAGIC_EXT` stamps the library with the Postgres version it was built for, and the server refuses to load a mismatch.
- **The V1 calling convention.** Every value crossing the SQL/C boundary is a `Datum`, and the `PG_GETARG_*` / `PG_RETURN_*` macros pack and unpack them.
- **Errors don't return.** `ereport(ERROR)` longjmps out and aborts the transaction. No cleanup code needed, because everything was palloc'd in a memory context the abort destroys wholesale.
- **SPI** is how C runs SQL inside the server, the same machinery PL/pgSQL uses. With `$1` parameters the pet's name is data, not SQL, so Bobby Tables is just a pet.
- **Concurrency is settled by the index.** `ON CONFLICT DO NOTHING` makes the insert itself the authority on whether a pet exists, no racy check-then-insert.
- **The conversation is table state.** `tama.message` is owned by the extension but dumped as user data, just like the pet.
- **Ask the catalogs where you live.** The C resolves the extension's schema at call time instead of hardcoding `tama`, so a renamed schema doesn't break anything.

And now the pet responds to how the database is treated.

```
postgres=# SELECT tama.status();
                          status
-----------------------------------------------------------
  (\_/)
  (o.o)  Ticktock
  (" ")  hunger 22/100  happiness 80/100  stress 0  poop 6

postgres=# CREATE TABLE junk AS SELECT generate_series(1,1000) g;
postgres=# DELETE FROM junk;
-- check in later, poop = 1006

postgres=# VACUUM junk;
-- check in later, poop = 6
```

Leave a transaction hanging open in another terminal and stress climbs. Commit it and the pet calms down. The pet is a database monitor with feelings.

### Makefile

PGXS, short for PostgreSQL Extension Building Infrastructure, is a Make include that ships with Postgres. It knows the server's exact compiler flags, install paths, and pg_regress invocations, so an extension's own Makefile only needs to declare a handful of variables and include PGXS at the bottom.

```make
MODULE_big = pg_tamagotchi
OBJS = src/pg_tamagotchi.o
EXTENSION = pg_tamagotchi
DATA = pg_tamagotchi--0.1.0.sql
PGFILEDESC = "pg_tamagotchi - a tamagotchi that lives in your database"
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

## Minting names

One quality-of-life feature before the pet gets its looks: a nameless egg should still hatch.

`tama.hatch()` with no argument mints a pronounceable name by gluing syllables together, an onset, a vowel, sometimes a closing consonant. The randomness comes from `pg_prng`, the random number generator Postgres itself ships.

```c
/* Syllable parts for minting names. Every combination is pronounceable. */
static const char *const name_onsets[] = {
  "b", "c", "d", "f", "g", "h", "j", "k", "l", "m", "n",
  "p", "r", "s", "t", "v", "w", "z", "br", "cr", "dr",
  "fr", "gr", "pr", "tr", "st", "sl", "sp", "th", "ch"
};
static const char *const name_vowels[] = {
  "a", "e", "i", "o", "u", "ai", "ee", "oo", "ou", "ia"
};
static const char *const name_codas[] = {"n", "r", "s", "t", "l", "k", "m"};

#define NELEMS(a) ((int) (sizeof(a) / sizeof((a)[0])))

/* Uniform random index in [0, n). */
static int pick(int n) {
  return (int) pg_prng_uint64_range(&pg_global_prng_state, 0, n - 1);
}

/* Mint a pronounceable name, two or three syllables, capitalized. */
static char *tama_random_name(void) {
  StringInfoData buf;
  int syllables = 2 + pick(2);

  initStringInfo(&buf);
  for (int i = 0; i < syllables; i++) {
    appendStringInfoString(&buf, name_onsets[pick(NELEMS(name_onsets))]);
    appendStringInfoString(&buf, name_vowels[pick(NELEMS(name_vowels))]);
  }
  if (pg_prng_double(&pg_global_prng_state) < 0.5) {
    appendStringInfoString(&buf, name_codas[pick(NELEMS(name_codas))]);
  }

  buf.data[0] = pg_toupper((unsigned char) buf.data[0]);

  return buf.data;
}
```

Praitreesleet, Broojar, Jeehee, Traboolai, Stotuzil and Saiprupai have all been born this way.
