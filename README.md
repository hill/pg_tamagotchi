# pg_tamagotchi

A tamagotchi that lives in your Postgres database. Hatch it, name it, feed it, and try not to let a restore kill it.

## Requirements

PostgreSQL 18 or newer. On macOS, `brew install postgresql@18`.

## Install

Build from source with PGXS, pointing `PG_CONFIG` at the server the pet will live in.

```bash
make install PG_CONFIG=/opt/homebrew/opt/postgresql@18/bin/pg_config
```

## The background worker

The pet ages on a timer driven by a background worker. Enable it by adding the library to your cluster's `postgresql.conf` and restarting.

```
shared_preload_libraries = 'pg_tamagotchi'
pg_tamagotchi.tick_interval = '10s'   # how often the pet's state advances
pg_tamagotchi.database = 'postgres'   # the database the pet lives in
```

Without the worker the extension still loads and the SQL functions work, the pet just doesn't age.

## Care instructions

```sql
CREATE EXTENSION pg_tamagotchi;

SELECT tama.status();          -- a speckled egg, waiting
SELECT tama.hatch('Blobby');   -- every pet needs a name (optionally leave blank and it will be invented)
SELECT tama.status();          -- check in on them
```

One pet per database, and it's communal, any role can care for it. Hatching a second is refused.

Watch them live from psql.

```
SELECT tama.status();
\watch 5
```

## Development

The justfile drives a throwaway cluster in `./pgdata` on port 5499, so your real Postgres is never touched.

```bash
just install   # build and install via PGXS
just init      # one-time, create the dev cluster
just start     # start it
just psql      # connect
just dev       # rebuild, reinstall, restart after C changes
just test      # run the pg_regress suite
just nuke      # delete the cluster
```
