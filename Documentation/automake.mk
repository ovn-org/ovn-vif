DOC_SOURCE = \
	Documentation/_static/logo.png \
	Documentation/index.rst \
	Documentation/topics/index.rst \
	Documentation/topics/vif-plug-providers/index.rst \
	Documentation/topics/vif-plug-providers/vif-plug-representor.rst

EXTRA_DIST += $(DOC_SOURCE)

# You can set these variables from the command line.
SPHINXOPTS =
SPHINXBUILD = sphinx-build
SPHINXSRCDIR = $(srcdir)/Documentation
SPHINXBUILDDIR = $(builddir)/Documentation/_build

# Internal variables.
ALLSPHINXOPTS = -W -n -d $(SPHINXBUILDDIR)/doctrees $(SPHINXOPTS) $(SPHINXSRCDIR)

sphinx_verbose = $(sphinx_verbose_@AM_V@)
sphinx_verbose_ = $(sphinx_verbose_@AM_DEFAULT_V@)
sphinx_verbose_0 = -q

if HAVE_SPHINX
docs-check: $(DOC_SOURCE)
	$(AM_V_GEN)$(SPHINXBUILD) $(sphinx_verbose) -b html $(ALLSPHINXOPTS) $(SPHINXBUILDDIR)/html && touch $@
	$(AM_V_GEN)$(SPHINXBUILD) $(sphinx_verbose) -b man $(ALLSPHINXOPTS) $(SPHINXBUILDDIR)/man && touch $@
ALL_LOCAL += docs-check
CLEANFILES += docs-check

check-docs:
	$(SPHINXBUILD) -b linkcheck $(ALLSPHINXOPTS) $(SPHINXBUILDDIR)/linkcheck

clean-docs:
	rm -rf $(SPHINXBUILDDIR)
	rm -f docs-check
CLEAN_LOCAL += clean-docs
endif
.PHONY: check-docs
.PHONY: clean-docs
