%.pdf: %.md
	pandoc $^ -o $@ --pdf-engine=lualatex

THEME = white
%.html: %.md
	pandoc $^ -o $@ --to revealjs --standalone --variable revealjs-url=https://cdn.jsdelivr.net/npm/reveal.js --variable theme=$(THEME)

all: spat-pgextday.html spat-pgextday.pdf
