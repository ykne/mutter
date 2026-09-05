# Copr "make_srpm" SCM source method entry point. Invoked by Copr's
# builder as `make -f .copr/Makefile srpm outdir=<dir> spec=<path>`,
# already inside a clone of this repo at the configured commit/branch.
# Produces an SRPM for `outdir` from HEAD, using the spec+patches
# committed alongside this Makefile under .copr/. Mirrors what
# make-source-tarball.sh + build-all.sh's SRPM step do in the local/CI
# pipeline (see github.com/ykne/GNOME-X11-specs), scoped to this package.

PKG := mutter
VERSION := 50.4

NEEDS_LOCAL_REPO :=
MOCK_CHROOT ?= fedora-44-x86_64
LOCAL_REPO ?= $(HOME)/gnome-x11-build/copr/RPMS

.PHONY: srpm
srpm:
	set -e; \
	command -v git >/dev/null 2>&1 || dnf install -y git-core; \
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

# Local-only: build the SRPM (above) into a binary RPM via mock, then
# publish it into a shared local repo so sibling packages' local builds
# can pick it up. Copr never calls this target - it gets equivalent
# cross-package visibility for free via its own project repo.
.PHONY: local-build
local-build: srpm
	set -e; \
	mkdir -p "$(LOCAL_REPO)/x86_64" "$(LOCAL_REPO)/noarch"; \
	[ -f "$(LOCAL_REPO)/repodata/repomd.xml" ] || createrepo_c "$(LOCAL_REPO)"; \
	srpm=$$(ls -t "$(outdir)"/$(PKG)-$(VERSION)-*.src.rpm | head -n1); \
	resultdir=$$(mktemp -d); \
	physical_cores=$$(lscpu | awk -F: '/^Core\(s\) per socket/{c=$$2} /^Socket\(s\)/{s=$$2} END{gsub(/ /,"",c); gsub(/ /,"",s); print c*s}'); \
	mock -r $(MOCK_CHROOT) --resultdir="$$resultdir" \
		--define "_smp_mflags -j$${physical_cores}" \
		$(if $(NEEDS_LOCAL_REPO),--addrepo="file://$(LOCAL_REPO)",) \
		--rebuild "$$srpm"; \
	cp "$$resultdir"/*.x86_64.rpm "$(LOCAL_REPO)/x86_64/" 2>/dev/null || true; \
	cp "$$resultdir"/*.noarch.rpm "$(LOCAL_REPO)/noarch/" 2>/dev/null || true; \
	createrepo_c "$(LOCAL_REPO)"; \
	rm -rf "$$resultdir"
