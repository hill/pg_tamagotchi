# pg_tamagotchi

<p align="center">
  <img src="assets/readme-hero.png" alt="pg_tamagotchi PostgreSQL elephant virtual pet" />
</p>

A tamagotchi that lives in your Postgres database. Hatch it, name it, feed it and don't kill it!

## Requirements

PostgreSQL 18 or newer. On macOS, `brew install postgresql@18`.

## Install

Build from source with PGXS, pointing `PG_CONFIG` at the server the pet will live in.

```bash
make install PG_CONFIG=/opt/homebrew/opt/postgresql@18/bin/pg_config
```

## Care instructions

```sql
CREATE EXTENSION pg_tamagotchi;

SELECT tama.status();          -- a speckled egg, waiting
SELECT tama.hatch('Ludo');     -- every pet needs a name (optionally leave blank and it will be invented)
SELECT tama.feed('apple');     -- snacks make them happy
SELECT tama.talk('hello?');    -- they talk back
SELECT tama.status();          -- check in on them
```

One pet per database, and it's communal, any role can care for it. Hatching a second is refused.
Talking stores both sides of the exchange in `tama.message`, so the conversation survives backup and restore with the pet.

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
