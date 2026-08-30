# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# Needs a TeX distribution with IEEEtran, which ships in texlive-publishers:
#   sudo apt install texlive-latex-recommended texlive-publishers \
#                    texlive-fonts-recommended
#
# latexmk drives the pdflatex/bibtex/pdflatex/pdflatex cycle itself, so the
# bibliography and the cross-references settle without a manual rerun.

PAPER := main

.PHONY: all clean watch arxiv

all: $(PAPER).pdf

$(PAPER).pdf: $(PAPER).tex refs.bib
	latexmk -pdf -halt-on-error -interaction=nonstopmode $(PAPER).tex

# Rebuild on every save, for drafting.
watch:
	latexmk -pdf -pvc -halt-on-error -interaction=nonstopmode $(PAPER).tex

# arXiv wants the .bbl rather than the .bib, since it does not run bibtex.
arxiv: $(PAPER).pdf
	tar czf arxiv-submission.tar.gz $(PAPER).tex $(PAPER).bbl
	@echo "wrote arxiv-submission.tar.gz"

clean:
	latexmk -C
	rm -f arxiv-submission.tar.gz
