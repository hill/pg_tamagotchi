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
    last_tick_at    timestamptz NOT NULL DEFAULT clock_timestamp(),
    -- Cumulative cache counters at the last tick. The mood reads the
    -- change since, not the lifetime average, so it tracks recent activity.
    prev_blks_hit   bigint NOT NULL DEFAULT 0,
    prev_blks_read  bigint NOT NULL DEFAULT 0
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

-- A friendly summary of the pet. Reflects the most recent tick, so call
-- tama.status() first if you want it freshly aged. The mood thresholds
-- match the faces drawn by status().
CREATE VIEW vitals AS
SELECT
    name,
    clock_timestamp() - born_at        AS age,
    hunger,
    happiness,
    poop                               AS dead_tuples,
    stress                             AS idle_in_transaction,
    round(cache_hit_ratio::numeric, 4) AS cache_hit_ratio,
    CASE
        WHEN hunger >= 80    THEN 'hungry'
        WHEN stress > 0      THEN 'stressed'
        WHEN poop >= 50      THEN 'grubby'
        WHEN happiness >= 90 THEN 'delighted'
        WHEN happiness <= 30 THEN 'sad'
        ELSE 'content'
    END                                AS mood,
    last_tick_at
FROM pet;

-- The pet is communal. The functions run with the caller's privileges,
-- so everyone needs real access to the schema and the table.
GRANT USAGE ON SCHEMA @extschema@ TO PUBLIC;
GRANT SELECT, INSERT, UPDATE ON pet TO PUBLIC;
GRANT SELECT, INSERT ON message TO PUBLIC;
GRANT SELECT ON vitals TO PUBLIC;
