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

PGDATA = ./pgdata
.PHONY: restart-db
restart-db:
	pg_ctl -D $(PGDATA) restart

dev: restart-db clean all install installcheck