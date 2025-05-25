EXTENSION = spat
EXTVERSION = 0.1.0a5

MODULE_big = $(EXTENSION)
OBJS = src/spat.o
HEADERS = src/spat.h

DATA = sql/spat--0.1.0a5.sql

PG_CPPFLAGS =

ifdef WITH_MURMUR3
OBJS += src/murmur3.o
PG_CPPFLAGS += -DSPAT_MURMUR3=1
endif

PG_CFLAGS = -Wno-unused-function -Wno-unused-variable -Wno-declaration-after-statement

TESTS = $(wildcard test/regress/sql/*.sql)
REGRESS = $(patsubst test/regress/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test/regress --load-extension=$(EXTENSION)

EXTRA_CLEAN = *.log dist gprof *.c.BAK *.html *.pdf *.rdb

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

######### DIST / RELEASE #########

.PHONY: dist

dist:
	mkdir -p dist
	git archive --format zip --prefix=$(EXTENSION)-$(EXTVERSION)/ --output dist/$(EXTENSION)-$(EXTVERSION).zip main

# for Docker
PG_MAJOR ?= 17

.PHONY: docker-build docker-release

docker-build:
	docker build --pull --no-cache --build-arg PG_MAJOR=$(PG_MAJOR) \
		-t florents/spat:pg$(PG_MAJOR) \
		-t florents/spat:$(EXTVERSION)-pg$(PG_MAJOR) \
		-t florents/spat:latest .

docker-release:
	docker buildx build --push --pull --no-cache --platform linux/amd64,linux/arm64 --build-arg PG_MAJOR=$(PG_MAJOR) \
		-t florents/spat:pg$(PG_MAJOR) \
		-t florents/spat:$(EXTVERSION)-pg$(PG_MAJOR) \
		-t florents/spat:latest .
######### DEVELOPMENT #########

PGDATA = ./pgdata
PG_CTL = pg_ctl
.PHONY: restart-db
restart-db:
	$(PG_CTL) -D $(PGDATA) restart

stop-db:
	$(PG_CTL) -D $(PGDATA) stop

start-db:
	postgres -D $(PGDATA)

dev: restart-db uninstall clean all install installcheck restart-db

ARTIFACTS = pgconf.dev.html pgconf.dev.pdf
%.pdf: %.md
	pandoc $^ -o $@ --pdf-engine=lualatex

THEME = solarized
%.html: %.md
	pandoc $^ -o $@ --to revealjs --standalone --variable revealjs-url=https://cdn.jsdelivr.net/npm/reveal.js --variable theme=$(THEME)

pgext.day: spat-pgextday.html spat-pgextday.pdf
