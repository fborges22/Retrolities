# Retrolities - top-level build driver (run inside the "build" container).
#
#   make                 build every project for every platform
#   make dos             one platform: dos | win16 | win9x | winnt
#   make PROJECTS=hello  restrict to some projects
#   make V=1             show full compiler / linker command lines
#   make help            list targets, projects and platforms

PLATFORMS := dos win16 win9x winnt
PROJECTS  ?= $(sort $(patsubst src/%/project.mk,%,$(wildcard src/*/project.mk)))
TYPE      ?= console

.PHONY: all $(PLATFORMS) clean list help new test
all: $(PLATFORMS)

$(PLATFORMS):
	@for p in $(PROJECTS); do \
	  $(MAKE) --no-print-directory -f mk/rules.mk DIR=src/$$p PLATFORM=$@ || exit 1; \
	done

clean:
	rm -rf build

list:
	@echo "platforms: $(PLATFORMS)"
	@echo "projects:  $(PROJECTS)"

new:
	@tools/newproj.sh "$(NAME)" "$(TYPE)"

test:
	@tests/run.sh

help:
	@echo "make [all]                 build all projects for all platforms"
	@echo "make <platform>            build for one of: $(PLATFORMS)"
	@echo "make PROJECTS='a b' ...    restrict the build to some projects"
	@echo "make new NAME=x [TYPE=gui] scaffold src/x (TYPE: console | gui)"
	@echo "make list                  show platforms and projects"
	@echo "make clean                 remove ./build"
	@echo "make test                  smoke tests (run in the 'test' container)"
	@echo "make V=1 ...               verbose command lines"
