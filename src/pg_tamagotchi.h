#ifndef PG_TAMAGOTCHI_H
#define PG_TAMAGOTCHI_H

/* GUCs defined in pg_tamagotchi.c, read by the worker */
extern int tama_tick_interval;
extern char *tama_database;

/* pg_tamagotchi.c */
extern char *tama_pet_relname(void);

#endif
