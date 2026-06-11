-- the preloaded worker is registered and visible cluster-wide
SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'pg_tamagotchi';

-- it lives in the database the GUC points it at
SELECT datname FROM pg_stat_activity WHERE backend_type = 'pg_tamagotchi';

-- both settings exist (loading the library defined them)
SELECT count(*) FROM pg_settings WHERE name LIKE 'pg_tamagotchi.%';
