ROOT := ../..
OUT := $(ROOT)/build/dos/bin/MAGICDSK.EXE
OBJ := $(ROOT)/build/dos/obj/magicdisk
WATCOM ?= /opt/watcom
WCC := $(WATCOM)/binl64/wcc386
WLINK := $(WATCOM)/binl64/wlink
SOURCES := main.c disk.c scan.c map.c bios.c dosraw.c bad.c repair.c journal.c recover.c text.c ui.c
OBJECTS := $(SOURCES:%.c=$(OBJ)/%.obj)
.PHONY: dos host test test-dos
dos: $(OUT)
$(OBJ) $(dir $(OUT)):
	mkdir -p $@
$(OBJ)/%.obj: %.c disk.h | $(OBJ)
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=$(WATCOM)/h -fo=$@ $<
$(OUT): $(OBJECTS) | $(dir $(OUT))
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k $(addprefix file ,$(OBJECTS))
host:
	mkdir -p $(ROOT)/build/host
	$(CC) -std=c99 -Wall -Wextra -Werror -g $(SOURCES) -o $(ROOT)/build/host/magicdisk
test: host
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_engine.c disk.c scan.c map.c -o $(ROOT)/build/host/magicdisk-engine-test
	$(ROOT)/build/host/magicdisk-engine-test
	python3 tests/test_images.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_bad_faults.c disk.c scan.c bad.c repair.c journal.c -o $(ROOT)/build/host/magicdisk-bad-test
	$(ROOT)/build/host/magicdisk-bad-test $(ROOT)/build/host/fault.mdj
	python3 tests/test_relocate.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_relocate_faults.c disk.c scan.c bad.c repair.c journal.c -o $(ROOT)/build/host/magicdisk-relocate-test
	$(ROOT)/build/host/magicdisk-relocate-test $(ROOT)/build/host/relocate.mdj
	python3 tests/test_bad.py $(ROOT)/build/host/magicdisk
	python3 tests/test_fsinfo.py $(ROOT)/build/host/magicdisk
	python3 tests/test_boot_backup.py $(ROOT)/build/host/magicdisk
	python3 tests/test_primary_boot.py $(ROOT)/build/host/magicdisk
	python3 tests/test_combined_metadata.py $(ROOT)/build/host/magicdisk
	python3 tests/test_mirrors.py $(ROOT)/build/host/magicdisk
	python3 tests/test_dot_pairs.py $(ROOT)/build/host/magicdisk
	python3 tests/test_dot_order.py $(ROOT)/build/host/magicdisk
	python3 tests/test_missing_dots.py $(ROOT)/build/host/magicdisk
	python3 tests/test_entries.py $(ROOT)/build/host/magicdisk
	python3 tests/test_dot_space.py $(ROOT)/build/host/magicdisk
	python3 tests/test_dot_lfn.py $(ROOT)/build/host/magicdisk
	python3 tests/test_directory_growth.py $(ROOT)/build/host/magicdisk
	python3 tests/test_torn_restore.py $(ROOT)/build/host/magicdisk
	python3 tests/test_directory_chains.py $(ROOT)/build/host/magicdisk
	python3 tests/test_shared_directories.py $(ROOT)/build/host/magicdisk
	python3 tests/test_shared_empty.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_directory_chain_faults.c disk.c scan.c bad.c repair.c journal.c -o $(ROOT)/build/host/magicdisk-directory-chain-test
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/directory-chain-fault.mdj
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/shared-prefix-fault.mdj --shared
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/empty-shared-fault.mdj --empty-shared
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/entry-fault.mdj --entries
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/missing-dot-fault.mdj --missing
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/dot-order-fault.mdj --order
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/dot-pair-fault.mdj --pairs
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/dot-space-fault.mdj --space
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/dot-lfn-fault.mdj --lfn
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/dot-child-fault.mdj --child
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/tree-child-fault.mdj --tree-child
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/tree-lfn-fault.mdj --tree-lfn
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/cross-child-fault.mdj --cross-child
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/cross-tree-fault.mdj --cross-tree
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/grow-child-fault.mdj --grow-child
	$(ROOT)/build/host/magicdisk-directory-chain-test $(ROOT)/build/host/grow-tree-fault.mdj --grow-tree
	python3 tests/test_chains.py $(ROOT)/build/host/magicdisk
	python3 tests/test_tree.py $(ROOT)/build/host/magicdisk
	python3 tests/test_tree_relocation.py $(ROOT)/build/host/magicdisk
	python3 tests/test_tree_source.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_chain_faults.c disk.c scan.c bad.c repair.c journal.c -o $(ROOT)/build/host/magicdisk-chain-test
	$(ROOT)/build/host/magicdisk-chain-test $(ROOT)/build/host/chain-fault.mdj
	$(ROOT)/build/host/magicdisk-chain-test $(ROOT)/build/host/tree-fault.mdj --tree
	$(ROOT)/build/host/magicdisk-chain-test $(ROOT)/build/host/source-fault.mdj --source
	$(ROOT)/build/host/magicdisk-chain-test $(ROOT)/build/host/tree-entry-fault.mdj --tree-entries
	$(ROOT)/build/host/magicdisk-chain-test $(ROOT)/build/host/source-entry-fault.mdj --source-entries
	python3 tests/test_reclaim.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_reclaim_faults.c disk.c scan.c bad.c repair.c journal.c -o $(ROOT)/build/host/magicdisk-reclaim-test
	$(ROOT)/build/host/magicdisk-reclaim-test $(ROOT)/build/host/reclaim-fault.mdj
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_recovery_faults.c disk.c scan.c recover.c -o $(ROOT)/build/host/magicdisk-recovery-test
	python3 tests/test_recovery.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-recovery-test
	python3 tests/test_live_export.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-recovery-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_duplicate_faults.c disk.c scan.c -o $(ROOT)/build/host/magicdisk-duplicate-test
	python3 tests/test_duplicate_names.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-duplicate-test
	python3 tests/test_directories.py $(ROOT)/build/host/magicdisk
	python3 tests/test_timestamps.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_text.c text.c -o $(ROOT)/build/host/magicdisk-text-test
	$(ROOT)/build/host/magicdisk-text-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_fsinfo_faults.c disk.c scan.c bad.c repair.c journal.c -o $(ROOT)/build/host/magicdisk-fsinfo-test
	$(ROOT)/build/host/magicdisk-fsinfo-test $(ROOT)/build/host/fsinfo-fault.mdj

$(ROOT)/build/dos/bin/KEYPLAY.EXE: tests/keyplay.c | $(OBJ) $(dir $(OUT))
	$(WATCOM)/binl64/wcc -zq -bt=dos -ms -0 -os -w4 -i=$(WATCOM)/h -fo=$(OBJ)/keyplay.obj $<
	$(WLINK) option quiet format dos option stack=2k libpath $(WATCOM)/lib286 libpath $(WATCOM)/lib286/dos name $@ file $(OBJ)/keyplay.obj
	python3 -c 'import struct; p="$@"; f=open(p,"r+b"); h=f.read(32); f.seek(12); f.write(struct.pack("<H",struct.unpack_from("<H",h,10)[0]+256)); f.close()'

$(ROOT)/build/dos/bin/LOCKTEST.EXE: tests/lockprobe.c | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=$(WATCOM)/h -fo=$(OBJ)/lockprobe.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=16k file $(OBJ)/lockprobe.obj

$(ROOT)/build/dos/bin/SECTGET.EXE: tests/sector_probe.c $(OBJ)/bios.obj $(OBJ)/disk.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=$(WATCOM)/h -fo=$(OBJ)/sector-probe.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=16k file $(OBJ)/sector-probe.obj file $(OBJ)/bios.obj file $(OBJ)/disk.obj

$(ROOT)/build/dos/bin/FREESPC.EXE: tests/free_space_probe.c | $(OBJ) $(dir $(OUT))
	$(WATCOM)/binl64/wcc -zq -bt=dos -ms -0 -os -w4 -i=$(WATCOM)/h -fo=$(OBJ)/free-space-probe.obj $<
	$(WLINK) option quiet format dos option stack=2k libpath $(WATCOM)/lib286 libpath $(WATCOM)/lib286/dos name $@ file $(OBJ)/free-space-probe.obj

.PHONY: test-compat
test-compat: host dos $(ROOT)/build/dos/bin/KEYPLAY.EXE $(ROOT)/build/dos/bin/FREESPC.EXE $(ROOT)/build/dos/bin/SECTGET.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622 or freedos)'; exit 1)
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --cache-workload
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 12
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 12 --write-protected
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --tree
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --tree --mounted-bits 12
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --shared-prefix
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --shared-prefix --mounted-bits 12
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --empty-alias
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --alias-ui
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --empty-alias --mounted-bits 12
	@if [ "$(DOS_KERNEL)" = freedos ]; then \
		python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 32 && \
		python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 32 --cache-workload && \
		python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 32 --tree && \
		python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 32 --shared-prefix && \
		python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 32 --empty-alias; \
	fi

# Both names run the same QEMU suite; a user-owned DOS boot disk is required.
.PHONY: test-win98
test-dos test-win98: host dos \
                    $(ROOT)/build/dos/bin/KEYPLAY.EXE \
                    $(ROOT)/build/dos/bin/LOCKTEST.EXE \
                    $(ROOT)/build/dos/bin/SECTGET.EXE \
                    $(ROOT)/build/dos/bin/FREESPC.EXE
	@test -n "$(WIN98_BOOT)" || (echo 'Set WIN98_BOOT to your Windows 98 boot image'; exit 1)
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)"
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --fsinfo
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --boot
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --mirror
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --recover
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --reclaim
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --chains
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --directory-chains
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --mixed
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --primary
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --tree
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --tree-source
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --progress
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --entries
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --missing-dots
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --dot-order
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --dot-pairs
	python3 tests/test_win98.py $(OUT) "$(WIN98_BOOT)" --viewer
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --shared-prefix --mounted-bits 12
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --shared-prefix --mounted-bits 16
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --shared-prefix --mounted-bits 32
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --empty-alias --mounted-bits 12
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --empty-alias --mounted-bits 16
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --alias-ui
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --empty-alias --mounted-bits 32
