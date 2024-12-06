EXTENSION = spat

MODULE_big = $(EXTENSION)
OBJS = spat.o
PGFILEDESC = "Description here"

DATA = spat--0.1.0.sql

TESTS = $(wildcard test/sql/*.sql)
REGRESS = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test --load-extension=$(EXTENSION)

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# targets used for development only

PGDATA = ./pgdata
PG_CTL = pg_ctl
.PHONY: restart-db
restart-db:
	$(PG_CTL) -D $(PGDATA) restart

stop-db:
	$(PG_CTL) -D $(PGDATA) stop

start-db:
	postgres -D $(PGDATA)

dev: restart-db clean all install installcheck