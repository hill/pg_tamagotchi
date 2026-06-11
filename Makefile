MODULE_big = pg_tamagotchi
OBJS = src/pg_tamagotchi.o src/worker.o
EXTENSION = pg_tamagotchi
DATA = pg_tamagotchi--0.1.0.sql
PGFILEDESC = "pg_tamagotchi - a tamagotchi that lives in your database"
# Only tests that pass on any cluster. The worker test needs
# shared_preload_libraries, so `just test` opts in with
# REGRESS="basic worker" against the dev cluster.
REGRESS = basic

# Apple clang doesn't know the syslog format archetype used by PG's
# printf attributes; without this every build emits 31 copies of the
# same harmless warning.
PG_CFLAGS = -Wno-ignored-attributes

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
