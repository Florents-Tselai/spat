ARG PG_MAJOR=17
FROM postgres:$PG_MAJOR
ARG PG_MAJOR

COPY . /tmp/spat

RUN apt-get update && \
		apt-mark hold locales && \
		apt-get install -y --no-install-recommends build-essential postgresql-server-dev-$PG_MAJOR && \
		cd /tmp/spat && \
		make clean && \
		make OPTFLAGS="" && \
		make install && \
		mkdir /usr/share/doc/spat && \
		cp LICENSE README.md /usr/share/doc/spat && \
		rm -r /tmp/spat && \
		apt-get remove -y build-essential postgresql-server-dev-$PG_MAJOR && \
		apt-get autoremove -y && \
		apt-mark unhold locales && \
		rm -rf /var/lib/apt/lists/*
