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

SELECT * FROM tama.vitals;     -- the numbers behind the mood
```

One pet per database, and it's communal, any role can care for it. Hatching a second is refused.

Watch them live from psql.

```
SELECT tama.status();
\watch 5
```

## See it react

The pet mirrors the health of its database. Dead tuples are poop, so make a mess and clean it up. The stats views are asynchronous, so give them a second to catch up before you check in.

```
postgres=# SELECT tama.hatch('Ludo');
 *crack*
  (\_/)
  (o.o)  Ludo hatched! Take good care of them.

postgres=# SELECT tama.status();
  (\_/)
  (o.o)
  (" ")
 Ludo is feeling content.
 hunger 20/100  happiness 80/100  stress 0  poop 0

postgres=# CREATE TABLE junk AS SELECT generate_series(1,1000);
postgres=# DELETE FROM junk;
postgres=# SELECT tama.status();   -- a second later
  (\_/)
  (-_-)
  (~~~)
 Ludo is feeling grubby.
 hunger 20/100  happiness 80/100  stress 0  poop 1001

postgres=# VACUUM junk;
postgres=# SELECT tama.status();   -- a second later
  (\_/)
  (o.o)
  (" ")
 Ludo is feeling content.
 hunger 20/100  happiness 80/100  stress 0  poop 2

postgres=# SELECT tama.feed('apple');
 Ludo munches the apple. hunger 0/100  happiness 85/100

postgres=# SELECT tama.talk('how are you?');
 you: how are you?
 Ludo says: That is a very good question for a tiny database pet.

postgres=# SELECT name, age, hunger, happiness, dead_tuples, mood FROM tama.vitals;
 name |   age    | hunger | happiness | dead_tuples |  mood
------+----------+--------+-----------+-------------+---------
 Ludo | 00:00:12 |      0 |        85 |           3 | content
```

Leave a transaction open in another session (`BEGIN; SELECT 1;`) and stress climbs, the eyes go wide. Let hunger build and the pet gets cranky. Feeding settles it.

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
