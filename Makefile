# Retrolities - top-level build driver (run inside the "build" container).
#
#   make                 build every project for every platform
#   make dos             one platform: dos | win16 | win9x | winnt
#   make os2             NetBolt for OS/2 2.0+
#   make PROJECTS=hello  restrict to some projects
#   make V=1             show full compiler / linker command lines
#   make help            list targets, projects and platforms

PLATFORMS := dos win16 win9x winnt
PROJECTS  ?= $(sort netbolt $(patsubst src/%/project.mk,%,$(wildcard src/*/project.mk)))
TYPE      ?= console

.PHONY: all $(PLATFORMS) os2 win64 netbolt-os2 netbolt-win64 picasso-win64 clean list help new test
all: $(PLATFORMS) $(if $(filter netbolt,$(PROJECTS)),netbolt-os2 netbolt-win64) \
	$(if $(filter picasso,$(PROJECTS)),picasso-win64)

$(PLATFORMS):
	@for p in $(PROJECTS); do \
	  if [ "$$p" = netbolt ]; then \
	    target=$@; [ "$$target" != win9x ] || target=win32; \
	    $(MAKE) --no-print-directory -C src/netbolt $$target || exit 1; \
	  else \
	    $(MAKE) --no-print-directory -f mk/rules.mk DIR=src/$$p PLATFORM=$@ || exit 1; \
	  fi; \
	done

win64: netbolt-win64 picasso-win64

netbolt-win64:
	@$(MAKE) --no-print-directory -C src/netbolt win64

picasso-win64:
	@$(MAKE) --no-print-directory -C src/picasso win64

os2:
	@$(MAKE) --no-print-directory -C src/netbolt os2

netbolt-os2: os2

clean:
	rm -rf build

list:
	@echo "platforms: $(PLATFORMS) (NetBolt also has os2; win64 builds NetBolt and Picasso)"
	@echo "projects:  $(PROJECTS)"

new:
	@tools/newproj.sh "$(NAME)" "$(TYPE)"

test:
	@tests/run.sh

help:
	@echo "make [all]                 build all projects for all platforms (including x64 Windows)"
	@echo "make <platform>            build for one of: $(PLATFORMS)"
	@echo "make os2                   build NetBolt for OS/2 2.0+"
	@echo "make win64                 build NetBolt and Picasso for x64 Windows"
	@echo "make PROJECTS='a b' ...    restrict the build to some projects"
	@echo "make new NAME=x [TYPE=gui] scaffold src/x (TYPE: console | gui)"
	@echo "make list                  show platforms and projects"
	@echo "make clean                 remove ./build"
	@echo "make test                  smoke tests (run in the 'test' container)"
	@echo "make V=1 ...               verbose command lines"
