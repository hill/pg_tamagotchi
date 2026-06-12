CREATE EXTENSION pg_tamagotchi;

-- an unhatched egg
SELECT tama.status();

-- the empty string is not a name
SELECT tama.hatch('');

SELECT tama.hatch('Blobby');

-- fresh pet vitals
SELECT tama.status();
SELECT name, hunger, happiness, poop, stress FROM tama.pet;

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
