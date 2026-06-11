\echo Use "CREATE EXTENSION pg_tamagotchi" to load this file. \quit

-- Single-row table holding the pet. The bool primary key with a CHECK
-- makes a second row impossible.
CREATE TABLE pet (
    only_one        bool PRIMARY KEY DEFAULT true CHECK (only_one),
    name            text NOT NULL DEFAULT 'Blobby',
    born_at         timestamptz NOT NULL DEFAULT now(),
    hunger          int NOT NULL DEFAULT 20 CHECK (hunger BETWEEN 0 AND 100),
    happiness       int NOT NULL DEFAULT 80 CHECK (happiness BETWEEN 0 AND 100),
    poop            bigint NOT NULL DEFAULT 0,
    stress          int NOT NULL DEFAULT 0,
    cache_hit_ratio float8,
    last_tick_at    timestamptz
);

INSERT INTO pet DEFAULT VALUES;

-- Pet state is user data, not extension furniture: include it in pg_dump
-- so the pet survives backup and restore.
SELECT pg_catalog.pg_extension_config_dump('pet', '');

CREATE FUNCTION status() RETURNS text
AS 'MODULE_PATHNAME', 'tama_status'
LANGUAGE C VOLATILE;
