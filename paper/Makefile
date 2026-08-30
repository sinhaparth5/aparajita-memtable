# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# Two engines, because the paper has to build on a machine without root. With a
# system TeX installed, latexmk drives the pdflatex/bibtex/pdflatex/pdflatex
# cycle itself:
#   sudo apt install texlive-latex-recommended texlive-publishers \
#                    texlive-fonts-recommended latexmk
# Without one, tectonic is a single static binary that fetches what it needs:
#   curl -sL https://drop-sh.fullyjustified.net | sh   # writes ./tectonic
# The committed PDF was built with tectonic 0.17.0.

PAPER := main

LATEXMK  := $(shell command -v latexmk 2>/dev/null)
TECTONIC := $(shell command -v tectonic 2>/dev/null)

ifdef LATEXMK
  BUILD = latexmk -pdf -halt-on-error -interaction=nonstopmode $(PAPER).tex
else
  BUILD = $(TECTONIC) -X compile --keep-intermediates --keep-logs $(PAPER).tex
endif

.PHONY: all clean watch arxiv

all: $(PAPER).pdf

$(PAPER).pdf: $(PAPER).tex refs.bib
ifeq ($(LATEXMK)$(TECTONIC),)
	@echo "no latexmk and no tectonic on PATH; see the header of this Makefile" >&2
	@false
endif
	$(BUILD)

# Rebuild on every save, for drafting. latexmk only.
watch:
	latexmk -pdf -pvc -halt-on-error -interaction=nonstopmode $(PAPER).tex

# arXiv wants the .bbl rather than the .bib, since it does not run bibtex.
arxiv: $(PAPER).pdf
	tar czf arxiv-submission.tar.gz $(PAPER).tex $(PAPER).bbl
	@echo "wrote arxiv-submission.tar.gz"

clean:
ifdef LATEXMK
	latexmk -C
else
	rm -f $(PAPER).pdf $(PAPER).aux $(PAPER).bbl $(PAPER).blg \
	      $(PAPER).log $(PAPER).out
endif
	rm -f arxiv-submission.tar.gz
