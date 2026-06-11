# pg_tamagotchi: Tamagotchi in postgres

[Postgres](https://www.tigerdata.com/blog/its-2026-just-use-postgres) [can](https://postgresforeverything.com/) [do](https://www.amazingcto.com/postgres-for-everything/) [everything](https://www.justfuckingusepostgres.com/)!

You don't need redis, elasticsearch, pinecone, arcgis or a $5000/month bill!

- Full text search = `pg_textsearch`
- Vector database = `pg_vector`
- Cache store = `UNLOGGED` tables
- NoSQL/documnents = `JSONB`
- Geospatial Database = `postgis`
- Queues = `pgmq`
- Cronjobs = `pg_cron`
- REST API = `postgREST`
- GraphQL = `postGraphile`
...

I love Postgres! But you know what's missing from this list? Tamagotchi implemented as a postgres extension. 

Therefore, introducing...

## pg_tamagotchi

pg_tamagotchi is your pet in postgres. You hatch it, name it, feed it, clean up it's shit and love and care for it. Furthermore, it integrates with your database statistics.


## Building Posgres Extensions