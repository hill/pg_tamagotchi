CREATE EXTENSION pg_tamagotchi;

-- an unhatched egg
SELECT tama.status();

-- the empty string is not a name
SELECT tama.hatch('');

SELECT tama.hatch('Blobby');

-- fresh pet vitals
SELECT tama.status() LIKE '%Blobby%' AS saw_pet;
SELECT name, hunger, happiness, poop >= 0 AS poop_recorded,
       stress >= 0 AS stress_recorded,
       last_tick_at IS NOT NULL AS clock_started
FROM tama.pet;

-- time passes when someone checks in
UPDATE tama.pet SET last_tick_at = clock_timestamp() - interval '25 seconds';
SELECT tama.status() LIKE '%hunger 22/100%' AS aged;
SELECT hunger FROM tama.pet;

-- feeding lowers hunger and makes the pet happier
SELECT tama.feed('apple') LIKE '%apple%' AS fed;
SELECT hunger, happiness FROM tama.pet;

-- talking stores both sides of the exchange
SELECT tama.talk('hello?') LIKE '%Blobby%' AS talked;
SELECT speaker, count(*) AS messages, bool_and(body <> '') AS has_body
FROM tama.message
GROUP BY speaker
ORDER BY speaker;

-- the vitals view summarises the pet without ticking it
SELECT name = 'Blobby' AS named,
       mood IN ('content', 'delighted', 'hungry', 'sad', 'stressed', 'grubby')
         AS has_mood,
       cache_hit_ratio IS NULL OR cache_hit_ratio BETWEEN 0 AND 1 AS ratio_sane,
       dead_tuples >= 0 AS poop_counted
FROM tama.vitals;

-- mood and face react to a hungry pet
UPDATE tama.pet SET hunger = 95;
SELECT tama.status() LIKE '%hungry%' AS looks_hungry;
SELECT mood FROM tama.vitals;

-- one pet per database, enforced in C and by the schema
SELECT tama.hatch('Impostor');
INSERT INTO tama.pet (name) VALUES ('Impostor');

-- drop takes every extension object with it, a fresh install gets a fresh egg
DROP EXTENSION pg_tamagotchi;
CREATE EXTENSION pg_tamagotchi;

-- names are data, not SQL
SELECT tama.hatch('Robert''); DROP TABLE tama.pet;--');
SELECT name FROM tama.pet;

-- a nameless egg gets a pronounceable name minted for it
DROP EXTENSION pg_tamagotchi;
CREATE EXTENSION pg_tamagotchi;
SELECT tama.hatch() LIKE '%hatched!%' AS hatched;
SELECT name ~ '^[A-Z][a-z]+$' AS pronounceable,
       length(name) BETWEEN 4 AND 13 AS reasonable
FROM tama.pet;
