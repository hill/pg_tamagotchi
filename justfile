# pg_tamagotchi dev commands
#
# Everything is pinned to the Homebrew postgresql@18 keg. The libpq keg also
# ships a pg_config that shadows this one on PATH but points at a nonexistent
# PGXS, so never rely on PATH lookup here.

pg_bin := "/opt/homebrew/opt/postgresql@18/bin"
pg_config := pg_bin + "/pg_config"
pgdata := "pgdata"
port := "5499"

default:
    @just --list

build:
    make PG_CONFIG={{pg_config}}

install: build
    make install PG_CONFIG={{pg_config}}

# Create the throwaway dev cluster
init:
    {{pg_bin}}/initdb -D {{pgdata}} -U $USER --no-locale -E UTF8
    echo "port = {{port}}" >> {{pgdata}}/postgresql.conf
    echo "listen_addresses = '127.0.0.1'" >> {{pgdata}}/postgresql.conf
    echo "log_min_messages = info" >> {{pgdata}}/postgresql.conf

start:
    {{pg_bin}}/pg_ctl -D {{pgdata}} -l {{pgdata}}/server.log start

stop:
    {{pg_bin}}/pg_ctl -D {{pgdata}} stop

restart:
    {{pg_bin}}/pg_ctl -D {{pgdata}} -l {{pgdata}}/server.log restart

reload:
    {{pg_bin}}/pg_ctl -D {{pgdata}} reload

# Rebuild, reinstall, restart. The full loop after a C change.
dev: install restart

psql *args:
    {{pg_bin}}/psql -h 127.0.0.1 -p {{port}} postgres {{args}}

log:
    tail -f {{pgdata}}/server.log

test: install
    PGHOST=127.0.0.1 PGPORT={{port}} make installcheck PG_CONFIG={{pg_config}}

clean:
    make clean PG_CONFIG={{pg_config}}

# Destroy the dev cluster entirely
nuke:
    -{{pg_bin}}/pg_ctl -D {{pgdata}} stop
    rm -rf {{pgdata}}
