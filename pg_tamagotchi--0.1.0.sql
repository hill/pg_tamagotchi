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

-- With no name, a pronounceable one is invented.
CREATE FUNCTION hatch(name text DEFAULT NULL) RETURNS text
AS 'MODULE_PATHNAME', 'tama_hatch'
LANGUAGE C VOLATILE;

CREATE FUNCTION status() RETURNS text
AS 'MODULE_PATHNAME', 'tama_status'
LANGUAGE C VOLATILE;

-- The pet is communal. The functions run with the caller's privileges,
-- so everyone needs real access to the schema and the table.
GRANT USAGE ON SCHEMA @extschema@ TO PUBLIC;
GRANT SELECT, INSERT, UPDATE ON pet TO PUBLIC;
