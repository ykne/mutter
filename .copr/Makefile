# Copr "make_srpm" SCM source method entry point. Invoked by Copr's
# builder as `make -f .copr/Makefile srpm outdir=<dir> spec=<path>`,
# already inside a clone of this repo at the configured commit/branch.
# Produces an SRPM for `outdir` from HEAD, using the spec+patches
# committed alongside this Makefile under .copr/. Mirrors what
# make-source-tarball.sh + build-all.sh's SRPM step do in the local/CI
# pipeline (see github.com/ykne/GNOME-X11-specs), scoped to this package.

PKG := mutter
VERSION := 50.4

.PHONY: srpm
srpm:
	set -e; \
	dnf install -y git-core; \
	hash=$$(git rev-parse --short=8 HEAD); \
	ts=$$(git log -1 --format=%ct HEAD); \
	workdir=$$(mktemp -d); \
	mkdir -p "$$workdir/$(PKG)-$(VERSION)" "$$workdir/SOURCES"; \
	git archive HEAD | tar -x -C "$$workdir/$(PKG)-$(VERSION)"; \
	tar -cJf "$$workdir/SOURCES/$(PKG)-$(VERSION).tar.xz" -C "$$workdir" "$(PKG)-$(VERSION)"; \
	cp .copr/SOURCES/* "$$workdir/SOURCES/" 2>/dev/null || true; \
	sed "s/^Release:.*\$$/Release:        $${ts}.g$${hash}%{?dist}/" "$(spec)" > "$$workdir/$(PKG).spec"; \
	rpmbuild -bs \
		--define "_topdir $$workdir" \
		--define "_sourcedir $$workdir/SOURCES" \
		--define "_specdir $$workdir" \
		--define "_builddir $$workdir/BUILD" \
		--define "_srcrpmdir $(outdir)" \
		"$$workdir/$(PKG).spec"; \
	rm -rf "$$workdir"
