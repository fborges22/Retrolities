ROOT := ../..
OUT := $(ROOT)/build/dos/bin/MAGICDSK.EXE
OBJ := $(ROOT)/build/dos/obj/magicdisk
WATCOM ?= /opt/watcom
WCC := $(WATCOM)/binl64/wcc386
WLINK := $(WATCOM)/binl64/wlink
SOURCES := main.c disk.c partition.c mbrjournal.c ebrjournal.c linkjournal.c containerjournal.c recordjournal.c scan.c map.c bios.c dosraw.c bad.c repair.c journal.c recover.c text.c xlink.c autofix.c cmos.c cvf.c ui.c
OBJECTS := $(SOURCES:%.c=$(OBJ)/%.obj)
.PHONY: dos host test test-dos
dos: $(OUT)
$(OBJ) $(dir $(OUT)):
	mkdir -p $@
$(OBJ)/scan.obj: namecase.h oemcode.h
$(OBJ)/%.obj: %.c disk.h | $(OBJ)
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=$(WATCOM)/h -fo=$@ $<
$(OUT): $(OBJECTS) | $(dir $(OUT))
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k $(addprefix file ,$(OBJECTS))
host:
	mkdir -p $(ROOT)/build/host
	$(CC) -std=c99 -Wall -Wextra -Werror -g $(SOURCES) -o $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -DMAGICDISK_SCRIPTED_UI $(SOURCES) -o $(ROOT)/build/host/magicdisk-ui
test: host test-mbr test-ebr test-ebr-link test-container test-record
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/volume_search_probe.c disk.c -o $(ROOT)/build/host/volume-search-probe
	python3 tests/test_volume_search.py $(ROOT)/build/host/volume-search-probe
	python3 tests/test_volume_search_cli.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_volume_search_bounds.c disk.c -o $(ROOT)/build/host/volume-search-bounds
	$(ROOT)/build/host/volume-search-bounds
	python3 tests/test_cli_numbers.py $(ROOT)/build/host/magicdisk
	python3 tests/test_oem_names.py $(ROOT)/build/host/magicdisk
	python3 tests/test_oem_extended.py $(ROOT)/build/host/magicdisk
	python3 tests/test_short_names.py $(ROOT)/build/host/magicdisk
	python3 tests/test_labels.py $(ROOT)/build/host/magicdisk
	python3 tests/test_label_controls.py $(ROOT)/build/host/magicdisk
	python3 tests/test_salvage.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_image_io.c disk.c -o $(ROOT)/build/host/magicdisk-image-io-test
	python3 tests/test_image_io.py $(ROOT)/build/host/magicdisk-image-io-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_plan_bounds.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-plan-bounds-test
	$(ROOT)/build/host/magicdisk-plan-bounds-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_engine.c disk.c scan.c map.c -o $(ROOT)/build/host/magicdisk-engine-test
	$(ROOT)/build/host/magicdisk-engine-test
	python3 tests/test_images.py $(ROOT)/build/host/magicdisk
	python3 tests/test_fat_headers.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_bad_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-bad-test
	$(ROOT)/build/host/magicdisk-bad-test $(ROOT)/build/host/fault.mdj
	python3 tests/test_relocate.py $(ROOT)/build/host/magicdisk
	python3 tests/test_directory_relocate.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_salvage_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-salvage-test
	$(ROOT)/build/host/magicdisk-salvage-test $(ROOT)/build/host/salvage.mdj
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_relocate_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-relocate-test
	$(ROOT)/build/host/magicdisk-relocate-test $(ROOT)/build/host/relocate.mdj
	$(ROOT)/build/host/magicdisk-relocate-test $(ROOT)/build/host/relocate.mdj --directory
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_root_relocate_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-root-relocate-test
	$(ROOT)/build/host/magicdisk-root-relocate-test $(ROOT)/build/host/root-relocate.mdj
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
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_directory_chain_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-directory-chain-test
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
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_xlink_faults.c disk.c scan.c map.c bad.c repair.c journal.c xlink.c autofix.c -o $(ROOT)/build/host/magicdisk-xlink-test
	python3 tests/test_crosslinks.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-xlink-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_cmos.c cmos.c -o $(ROOT)/build/host/magicdisk-cmos-test
	$(ROOT)/build/host/magicdisk-cmos-test $(ROOT)/build/host/magicdisk
	python3 tests/test_fix_ui.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-ui
	python3 tests/test_tree.py $(ROOT)/build/host/magicdisk
	python3 tests/test_tree_relocation.py $(ROOT)/build/host/magicdisk
	python3 tests/test_tree_source.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_chain_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-chain-test
	$(ROOT)/build/host/magicdisk-chain-test $(ROOT)/build/host/chain-fault.mdj
	$(ROOT)/build/host/magicdisk-chain-test $(ROOT)/build/host/tree-fault.mdj --tree
	$(ROOT)/build/host/magicdisk-chain-test $(ROOT)/build/host/source-fault.mdj --source
	$(ROOT)/build/host/magicdisk-chain-test $(ROOT)/build/host/tree-entry-fault.mdj --tree-entries
	$(ROOT)/build/host/magicdisk-chain-test $(ROOT)/build/host/source-entry-fault.mdj --source-entries
	python3 tests/test_reclaim.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_reclaim_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-reclaim-test
	$(ROOT)/build/host/magicdisk-reclaim-test $(ROOT)/build/host/reclaim-fault.mdj
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_recovery_faults.c disk.c scan.c recover.c -o $(ROOT)/build/host/magicdisk-recovery-test
	python3 tests/test_directory_gaps.py $(ROOT)/build/host/magicdisk-recovery-test
	python3 tests/test_recovery.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-recovery-test
	python3 tests/test_live_export.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-recovery-test
	python3 tests/test_selected_export.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-recovery-test
	python3 tests/test_deleted_recovery.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-recovery-test
	python3 tests/test_deleted_names.py $(ROOT)/build/host/magicdisk
	python3 tests/test_deleted_directories.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-recovery-test
	python3 tests/test_orphan_directories.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-recovery-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_duplicate_faults.c disk.c scan.c -o $(ROOT)/build/host/magicdisk-duplicate-test
	python3 tests/test_duplicate_names.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-duplicate-test
	python3 tests/test_directories.py $(ROOT)/build/host/magicdisk
	python3 tests/test_timestamps.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_timestamp_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-timestamp-test
	python3 tests/test_timestamp_repair.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-timestamp-test
	python3 tests/test_lfn_repair.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-timestamp-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_alias_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-alias-test
	python3 tests/test_alias_repair.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-alias-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_label_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-label-test
	python3 tests/test_label_repair.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-label-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_label_text_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-label-text-test
	python3 tests/test_label_text.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-label-text-test
	python3 tests/test_metadata_sequence.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-label-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_extra_dots_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-extra-dots-test
	python3 tests/test_extra_dots.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-extra-dots-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_attribute_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-attribute-test
	python3 tests/test_attribute_repair.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-attribute-test
	python3 tests/test_attribute_alias_sequence.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-attribute-test
	python3 tests/test_attribute_followups.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-attribute-test
	python3 tests/test_dot_attribute_sequence.py $(ROOT)/build/host/magicdisk $(ROOT)/build/host/magicdisk-attribute-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_conflict_query.c disk.c scan.c -o $(ROOT)/build/host/magicdisk-conflict-query-test
	python3 tests/test_conflict_query.py $(ROOT)/build/host/magicdisk-conflict-query-test
	python3 tests/test_conflict_repair.py $(ROOT)/build/host/magicdisk
	python3 tests/test_conflict_edges.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_conflict_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-conflict-fault-test
	python3 tests/test_conflict_faults.py $(ROOT)/build/host/magicdisk-conflict-fault-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_text.c text.c -o $(ROOT)/build/host/magicdisk-text-test
	$(ROOT)/build/host/magicdisk-text-test
	$(CC) -std=c99 -Wall -Wextra -Werror -g -I. tests/test_fsinfo_faults.c disk.c scan.c bad.c repair.c journal.c xlink.c -o $(ROOT)/build/host/magicdisk-fsinfo-test
	$(ROOT)/build/host/magicdisk-fsinfo-test $(ROOT)/build/host/fsinfo-fault.mdj

$(ROOT)/build/dos/bin/KEYPLAY.EXE: tests/keyplay.c | $(OBJ) $(dir $(OUT))
	$(WATCOM)/binl64/wcc -zq -bt=dos -ms -0 -os -w4 -i=$(WATCOM)/h -fo=$(OBJ)/keyplay.obj $<
	$(WLINK) option quiet format dos option stack=2k libpath $(WATCOM)/lib286 libpath $(WATCOM)/lib286/dos name $@ file $(OBJ)/keyplay.obj
	python3 -c 'import struct; p="$@"; f=open(p,"r+b"); h=f.read(32); f.seek(12); f.write(struct.pack("<H",struct.unpack_from("<H",h,10)[0]+256)); f.close()'

$(ROOT)/build/dos/bin/LOCKTEST.EXE: tests/lockprobe.c | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=$(WATCOM)/h -fo=$(OBJ)/lockprobe.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=16k file $(OBJ)/lockprobe.obj

$(ROOT)/build/dos/bin/IMAGEIO.EXE: tests/test_image_io.c disk.h $(OBJ)/disk.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/image-io.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=16k file $(OBJ)/image-io.obj file $(OBJ)/disk.obj

$(ROOT)/build/dos/bin/VSBOUND.EXE: tests/test_volume_search_bounds.c disk.h $(OBJ)/disk.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/volume-search-bounds.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/volume-search-bounds.obj file $(OBJ)/disk.obj

$(ROOT)/build/dos/bin/FATBOUND.EXE: tests/test_plan_bounds.c disk.h $(OBJ)/disk.obj $(OBJ)/scan.obj $(OBJ)/bad.obj $(OBJ)/repair.obj $(OBJ)/journal.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/plan-bounds.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/plan-bounds.obj file $(OBJ)/disk.obj file $(OBJ)/scan.obj file $(OBJ)/bad.obj file $(OBJ)/repair.obj file $(OBJ)/journal.obj

$(ROOT)/build/dos/bin/RELOCFLT.EXE: tests/test_relocate_faults.c disk.h $(OBJ)/disk.obj $(OBJ)/scan.obj $(OBJ)/bad.obj $(OBJ)/repair.obj $(OBJ)/journal.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/relocate-faults.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/relocate-faults.obj file $(OBJ)/disk.obj file $(OBJ)/scan.obj file $(OBJ)/bad.obj file $(OBJ)/repair.obj file $(OBJ)/journal.obj

# Synthetic media tests link the same map implementation as MagicDisk.
$(ROOT)/build/dos/bin/MAPTEST.EXE: tests/test_engine.c disk.h $(OBJ)/disk.obj $(OBJ)/scan.obj $(OBJ)/map.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=$(WATCOM)/h -fo=$(OBJ)/map-test.obj tests/test_engine.c
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/map-test.obj file $(OBJ)/disk.obj file $(OBJ)/scan.obj file $(OBJ)/map.obj

# Write primitives are available only in this disposable-media QEMU helper.
$(ROOT)/build/dos/bin/BIOSWRT.EXE: tests/bios_write_probe.c bios.c disk.h $(OBJ)/disk.obj $(OBJ)/dosraw.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -dMAGICDISK_BIOS_TEST_WRITES -i=$(WATCOM)/h -fo=$(OBJ)/bios-write-probe.obj tests/bios_write_probe.c
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -dMAGICDISK_BIOS_TEST_WRITES -i=$(WATCOM)/h -fo=$(OBJ)/bios-test-write.obj bios.c
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=16k file $(OBJ)/bios-write-probe.obj file $(OBJ)/bios-test-write.obj file $(OBJ)/disk.obj file $(OBJ)/dosraw.obj

$(ROOT)/build/dos/bin/BIOSJNL.EXE: tests/bios_journal_probe.c bios.c disk.h $(OBJ)/disk.obj $(OBJ)/scan.obj $(OBJ)/bad.obj $(OBJ)/repair.obj $(OBJ)/journal.obj $(OBJ)/dosraw.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -dMAGICDISK_BIOS_TEST_WRITES -i=$(WATCOM)/h -fo=$(OBJ)/bios-journal-probe.obj tests/bios_journal_probe.c
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -dMAGICDISK_BIOS_TEST_WRITES -i=$(WATCOM)/h -fo=$(OBJ)/bios-journal-write.obj bios.c
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/bios-journal-probe.obj file $(OBJ)/bios-journal-write.obj file $(OBJ)/disk.obj file $(OBJ)/scan.obj file $(OBJ)/bad.obj file $(OBJ)/repair.obj file $(OBJ)/journal.obj file $(OBJ)/dosraw.obj

$(ROOT)/build/dos/bin/BIOSCHK.EXE: tests/bios_preflight_probe.c disk.h $(OBJ)/bios.obj $(OBJ)/dosraw.obj $(OBJ)/disk.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=$(WATCOM)/h -fo=$(OBJ)/bios-preflight-probe.obj tests/bios_preflight_probe.c
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=16k file $(OBJ)/bios-preflight-probe.obj file $(OBJ)/bios.obj file $(OBJ)/dosraw.obj file $(OBJ)/disk.obj

$(ROOT)/build/dos/bin/SECTGET.EXE: tests/sector_probe.c $(OBJ)/bios.obj $(OBJ)/disk.obj $(OBJ)/dosraw.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=$(WATCOM)/h -fo=$(OBJ)/sector-probe.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=16k file $(OBJ)/sector-probe.obj file $(OBJ)/bios.obj file $(OBJ)/disk.obj file $(OBJ)/dosraw.obj

$(ROOT)/build/dos/bin/FREESPC.EXE: tests/free_space_probe.c | $(OBJ) $(dir $(OUT))
	$(WATCOM)/binl64/wcc -zq -bt=dos -ms -0 -os -w4 -i=$(WATCOM)/h -fo=$(OBJ)/free-space-probe.obj $<
	$(WLINK) option quiet format dos option stack=2k libpath $(WATCOM)/lib286 libpath $(WATCOM)/lib286/dos name $@ file $(OBJ)/free-space-probe.obj

$(ROOT)/build/dos/bin/FILECOPY.EXE: tests/file_copy_probe.c | $(OBJ) $(dir $(OUT))
	$(WATCOM)/binl64/wcc -zq -bt=dos -ms -0 -os -w4 -i=$(WATCOM)/h -fo=$(OBJ)/file-copy-probe.obj $<
	$(WLINK) option quiet format dos option stack=4k libpath $(WATCOM)/lib286 libpath $(WATCOM)/lib286/dos name $@ file $(OBJ)/file-copy-probe.obj

$(ROOT)/build/dos/bin/RECFAULT.EXE: tests/test_recovery_faults.c $(OBJ)/disk.obj $(OBJ)/scan.obj $(OBJ)/recover.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/recovery-fault.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/recovery-fault.obj file $(OBJ)/disk.obj file $(OBJ)/scan.obj file $(OBJ)/recover.obj

.PHONY: test-bios-writes
test-bios-writes: dos $(ROOT)/build/dos/bin/BIOSWRT.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_bios_writes.py $(ROOT)/build/dos/bin/BIOSWRT.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-bios-journal
test-bios-journal: dos $(ROOT)/build/dos/bin/BIOSJNL.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_bios_journal.py $(ROOT)/build/dos/bin/BIOSJNL.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-bios-cli
test-bios-cli: dos
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_bios_journal.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --production
	python3 tests/test_bios_journal.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --production --unsupported-controller

.PHONY: package test-package
package: dos
	python3 tools/package_dos.py $(OUT) $(ROOT)/build/dos/dist/MAGICDSK.ZIP --watcom "$(WATCOM)"

test-package: package $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_package_dos.py $(ROOT)/build/dos/dist/MAGICDSK.ZIP "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --keyplay $(ROOT)/build/dos/bin/KEYPLAY.EXE

.PHONY: test-fat-headers
test-fat-headers: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_fat_headers_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-display
test-display: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_display_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_surface_range_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-findings-capacity
test-findings-capacity: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_findings_capacity_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-short-names
test-short-names: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_short_names_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_oem_names_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_oem_extended_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-owner
test-owner: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_owner_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-lfn-cleanup
test-lfn-cleanup: host dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_lfn_repair.py $(ROOT)/build/host/magicdisk
	python3 tests/test_lfn_repair_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-orphan-directories
test-orphan-directories: host dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_orphan_directories.py $(ROOT)/build/host/magicdisk
	python3 tests/test_orphan_directories_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-directory-gaps
test-directory-gaps: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_directory_gaps_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_directory_gaps_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --root-gap --ui

.PHONY: test-salvage
test-salvage: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_salvage_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_salvage_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --torn-undo

.PHONY: test-labels
test-labels: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_labels_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_label_controls_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-image-io
test-image-io: dos $(ROOT)/build/dos/bin/IMAGEIO.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_image_io_dos.py $(ROOT)/build/dos/bin/IMAGEIO.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-plan-bounds
test-plan-bounds: dos $(ROOT)/build/dos/bin/FATBOUND.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_plan_bounds_dos.py $(ROOT)/build/dos/bin/FATBOUND.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-relocate-faults
test-relocate-faults: dos $(ROOT)/build/dos/bin/RELOCFLT.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_relocate_faults_dos.py $(ROOT)/build/dos/bin/RELOCFLT.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-map-dos
test-map-dos: dos $(ROOT)/build/dos/bin/MAPTEST.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_map_dos.py $(ROOT)/build/dos/bin/MAPTEST.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-retest
test-retest: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_retest_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-selected-export
test-selected-export: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_selected_export_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-finding-actions
test-finding-actions: host dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_finding_actions_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_finding_repairs_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_finding_repairs_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --commit-files

.PHONY: test-unicode
test-unicode: dos
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_unicode_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-bios-ui
test-bios-ui: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_bios_journal.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --production --tui

.PHONY: test-dos-map
test-dos-map: dos $(ROOT)/build/dos/bin/BIOSCHK.EXE $(ROOT)/build/dos/bin/KEYPLAY.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622, freedos or win98)'; exit 1)
	python3 tests/test_dos_map.py $(OUT) "$(DOS_BOOT)" --repair-refusals --kernel "$(DOS_KERNEL)" --exports --tui-exports --preflight $(ROOT)/build/dos/bin/BIOSCHK.EXE

.PHONY: test-bios-preflight
test-bios-preflight: test-dos-map
	@if [ -n "$(DOS_RAMDRIVE)" ]; then \
		python3 tests/test_dos_map.py $(OUT) "$(DOS_BOOT)" --repair-refusals --kernel "$(DOS_KERNEL)" --preflight $(ROOT)/build/dos/bin/BIOSCHK.EXE --ramdrive "$(DOS_RAMDRIVE)"; \
	fi

.PHONY: test-bios-cache
test-bios-cache: dos $(ROOT)/build/dos/bin/BIOSCHK.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_SMARTDRV)" -a -n "$(DOS_HIMEM)" || (echo 'Set DOS_BOOT, DOS_SMARTDRV and DOS_HIMEM to MS-DOS 6.22 files'; exit 1)
	python3 tests/test_dos_map.py $(OUT) "$(DOS_BOOT)" --repair-refusals --kernel msdos622 --preflight $(ROOT)/build/dos/bin/BIOSCHK.EXE --smartdrv "$(DOS_SMARTDRV)" --himem "$(DOS_HIMEM)"
	python3 tests/test_dos_map.py $(OUT) "$(DOS_BOOT)" --repair-refusals --kernel msdos622 --preflight $(ROOT)/build/dos/bin/BIOSCHK.EXE --smartdrv "$(DOS_SMARTDRV)" --himem "$(DOS_HIMEM)" --write-behind

.PHONY: test-compat
test-compat: host dos package $(ROOT)/build/dos/bin/VSBOUND.EXE $(ROOT)/build/dos/bin/RELOCFLT.EXE $(ROOT)/build/dos/bin/IMAGEIO.EXE $(ROOT)/build/dos/bin/FATBOUND.EXE $(ROOT)/build/dos/bin/MAPTEST.EXE $(ROOT)/build/dos/bin/BIOSCHK.EXE $(ROOT)/build/dos/bin/BIOSJNL.EXE $(ROOT)/build/dos/bin/BIOSWRT.EXE $(ROOT)/build/dos/bin/FILECOPY.EXE $(ROOT)/build/dos/bin/RECFAULT.EXE $(ROOT)/build/dos/bin/KEYPLAY.EXE $(ROOT)/build/dos/bin/FREESPC.EXE $(ROOT)/build/dos/bin/SECTGET.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL (msdos622 or freedos)'; exit 1)
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --relocate-native --retry-faults --timestamps --deleted-recovery
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --deleted-directories
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --deleted-missing-dots
	python3 tests/test_unicode_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_short_names_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_oem_names_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_oem_extended_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_owner_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_orphan_directories_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_lfn_repair_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_alias_repair_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_label_repair_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_volume_search_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --ui --bounds $(ROOT)/build/dos/bin/VSBOUND.EXE
	python3 tests/test_volume_search_display.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --captures $(ROOT)/build/magicdisk-test/discovery-display/$(DOS_KERNEL)
	python3 tests/test_label_text_mounted_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_label_text_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_metadata_sequence_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_extra_dots_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_attribute_repair_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_attribute_alias_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_attribute_followups_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --kind conflicts
	python3 tests/test_attribute_followups_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --kind labels
	python3 tests/test_dot_attribute_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_conflict_repair_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_alias_mounted_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_menu_layout_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_directory_gaps_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_directory_gaps_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --root-gap --ui
	python3 tests/test_salvage_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_salvage_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --torn-undo
	python3 tests/test_labels_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_label_controls_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_fat_headers_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_package_dos.py $(ROOT)/build/dos/dist/MAGICDSK.ZIP "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --keyplay $(ROOT)/build/dos/bin/KEYPLAY.EXE
	python3 tests/test_map_dos.py $(ROOT)/build/dos/bin/MAPTEST.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_plan_bounds_dos.py $(ROOT)/build/dos/bin/FATBOUND.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_relocate_faults_dos.py $(ROOT)/build/dos/bin/RELOCFLT.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_image_io_dos.py $(ROOT)/build/dos/bin/IMAGEIO.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_retest_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_finding_actions_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_finding_repairs_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_finding_repairs_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --commit-files
	python3 tests/test_display_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_surface_range_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_findings_capacity_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_selected_export_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_bios_writes.py $(ROOT)/build/dos/bin/BIOSWRT.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_bios_journal.py $(ROOT)/build/dos/bin/BIOSJNL.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_bios_journal.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --production
	python3 tests/test_bios_journal.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --production --tui
	python3 tests/test_dos_map.py $(OUT) "$(DOS_BOOT)" --repair-refusals --kernel "$(DOS_KERNEL)" --exports --tui-exports --preflight $(ROOT)/build/dos/bin/BIOSCHK.EXE
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --relocate-directory
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --relocate-directory --mounted-bits 12
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --cache-workload
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 12 --relocate-native
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 12 --write-protected
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --tree
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --tree --mounted-bits 12
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --shared-prefix
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --shared-prefix --mounted-bits 12
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --empty-alias
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --alias-ui
	python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --empty-alias --mounted-bits 12
	@if [ "$(DOS_KERNEL)" = freedos ]; then \
		python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 32 --relocate-native && \
		python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 32 --relocate-directory && \
		python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 32 --relocate-root && \
		python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 32 --relocate-root --relocate-torn-undo && \
		python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 32 --cache-workload && \
		python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 32 --tree && \
		python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 32 --shared-prefix && \
		python3 tests/test_dos_compat.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)" --mounted-bits 32 --empty-alias; \
	fi

# Both names run the same QEMU suite; a user-owned DOS boot disk is required.
.PHONY: test-win98
test-dos test-win98: host dos package $(ROOT)/build/dos/bin/VSBOUND.EXE $(ROOT)/build/dos/bin/RELOCFLT.EXE $(ROOT)/build/dos/bin/IMAGEIO.EXE $(ROOT)/build/dos/bin/FATBOUND.EXE $(ROOT)/build/dos/bin/MAPTEST.EXE $(ROOT)/build/dos/bin/BIOSCHK.EXE $(ROOT)/build/dos/bin/BIOSJNL.EXE $(ROOT)/build/dos/bin/BIOSWRT.EXE $(ROOT)/build/dos/bin/FILECOPY.EXE $(ROOT)/build/dos/bin/RECFAULT.EXE \
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
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --relocate-native --mounted-bits 12
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --relocate-native --mounted-bits 16 --retry-faults --timestamps --deleted-recovery
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --deleted-directories
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --deleted-missing-dots
	python3 tests/test_unicode_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_short_names_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_oem_names_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_oem_extended_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_owner_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_orphan_directories_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_lfn_repair_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_alias_repair_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_label_repair_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_volume_search_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --ui --bounds $(ROOT)/build/dos/bin/VSBOUND.EXE
	python3 tests/test_volume_search_display.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --captures $(ROOT)/build/magicdisk-test/discovery-display/win98
	python3 tests/test_label_text_mounted_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_label_text_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_metadata_sequence_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_extra_dots_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_attribute_repair_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_attribute_alias_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_attribute_followups_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --kind conflicts
	python3 tests/test_attribute_followups_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --kind labels
	python3 tests/test_dot_attribute_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_conflict_repair_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_alias_mounted_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_menu_layout_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_directory_gaps_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_directory_gaps_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --root-gap --ui
	python3 tests/test_salvage_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_salvage_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --torn-undo
	python3 tests/test_labels_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_label_controls_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_fat_headers_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_package_dos.py $(ROOT)/build/dos/dist/MAGICDSK.ZIP "$(WIN98_BOOT)" --kernel win98 --keyplay $(ROOT)/build/dos/bin/KEYPLAY.EXE
	python3 tests/test_image_io_dos.py $(ROOT)/build/dos/bin/IMAGEIO.EXE "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_plan_bounds_dos.py $(ROOT)/build/dos/bin/FATBOUND.EXE "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_relocate_faults_dos.py $(ROOT)/build/dos/bin/RELOCFLT.EXE "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_map_dos.py $(ROOT)/build/dos/bin/MAPTEST.EXE "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_selected_export_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_display_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_surface_range_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_findings_capacity_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_finding_actions_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_finding_repairs_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_finding_repairs_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --commit-files
	python3 tests/test_retest_dos.py $(OUT) "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_bios_writes.py $(ROOT)/build/dos/bin/BIOSWRT.EXE "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_bios_journal.py $(ROOT)/build/dos/bin/BIOSJNL.EXE "$(WIN98_BOOT)" --kernel win98
	python3 tests/test_bios_journal.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --production
	python3 tests/test_bios_journal.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --production --tui
	python3 tests/test_dos_map.py $(OUT) "$(WIN98_BOOT)" --repair-refusals --kernel win98 --exports --tui-exports --preflight $(ROOT)/build/dos/bin/BIOSCHK.EXE
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --relocate-native --mounted-bits 32
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --relocate-directory --mounted-bits 12
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --relocate-directory --mounted-bits 16
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --relocate-directory --mounted-bits 32
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --relocate-root --mounted-bits 32
	python3 tests/test_dos_compat.py $(OUT) "$(WIN98_BOOT)" --kernel win98 --relocate-root --mounted-bits 32 --relocate-torn-undo

$(ROOT)/build/dos/bin/MBRPLAN.EXE: tests/mbr_plan_probe.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/bios.obj $(OBJ)/dosraw.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/mbr-plan-probe.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/mbr-plan-probe.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/bios.obj file $(OBJ)/dosraw.obj

$(ROOT)/build/dos/bin/MBRTXN.EXE: tests/mbr_txn_probe.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/mbrjournal.obj $(OBJ)/bios.obj $(OBJ)/dosraw.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/mbr-txn-probe.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/mbr-txn-probe.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/mbrjournal.obj file $(OBJ)/bios.obj file $(OBJ)/dosraw.obj

.PHONY: test-mbr
test-mbr: host
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/mbr_plan_probe.c disk.c partition.c bios.c dosraw.c -o $(ROOT)/build/host/mbr-plan-probe
	python3 tests/test_mbr_plan.py $(ROOT)/build/host/mbr-plan-probe $(ROOT)/build/host/magicdisk
	python3 tests/test_mbr_cli.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/test_mbr_plan_faults.c disk.c partition.c -o $(ROOT)/build/host/mbr-faults
	$(ROOT)/build/host/mbr-faults
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/test_mbr_journal.c disk.c partition.c mbrjournal.c -o $(ROOT)/build/host/mbr-journal-test
	$(ROOT)/build/host/mbr-journal-test $(ROOT)/build/host/mbr-test.mdj

$(ROOT)/build/dos/bin/MBRJCHK.EXE: tests/test_mbr_journal.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/mbrjournal.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/mbr-journal-test.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/mbr-journal-test.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/mbrjournal.obj

.PHONY: test-mbr-dos
test-mbr-dos: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE $(ROOT)/build/dos/bin/MBRJCHK.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL'; exit 1)
	python3 tests/test_mbr_cli_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_mbr_ui_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_mbr_reboot_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_mbr_faults_dos.py $(ROOT)/build/dos/bin/MBRJCHK.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

$(ROOT)/build/dos/bin/EBRPLAN.EXE: tests/ebr_plan_probe.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/bios.obj $(OBJ)/dosraw.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/ebr-plan-probe.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/ebr-plan-probe.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/bios.obj file $(OBJ)/dosraw.obj

.PHONY: test-ebr
test-ebr: host
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/ebr_plan_probe.c disk.c partition.c bios.c dosraw.c -o $(ROOT)/build/host/ebr-plan-probe
	python3 tests/test_ebr_plan.py $(ROOT)/build/host/ebr-plan-probe $(ROOT)/build/host/magicdisk
	python3 tests/test_ebr_cli.py $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/test_ebr_faults.c disk.c partition.c -o $(ROOT)/build/host/ebr-faults
	$(ROOT)/build/host/ebr-faults
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/test_ebr_journal.c disk.c partition.c ebrjournal.c -o $(ROOT)/build/host/ebr-journal-test
	$(ROOT)/build/host/ebr-journal-test $(ROOT)/build/host/ebr-test.mdj

$(ROOT)/build/dos/bin/EBRTXN.EXE: tests/ebr_txn_probe.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/ebrjournal.obj $(OBJ)/bios.obj $(OBJ)/dosraw.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/ebr-txn-probe.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/ebr-txn-probe.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/ebrjournal.obj file $(OBJ)/bios.obj file $(OBJ)/dosraw.obj

$(ROOT)/build/dos/bin/EBRJCHK.EXE: tests/test_ebr_journal.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/ebrjournal.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/ebr-journal-test.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/ebr-journal-test.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/ebrjournal.obj

.PHONY: test-ebr-dos
test-ebr-dos: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE $(ROOT)/build/dos/bin/EBRJCHK.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL'; exit 1)
	python3 tests/test_ebr_cli_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_ebr_ui_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_ebr_reboot_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_ebr_faults_dos.py $(ROOT)/build/dos/bin/EBRJCHK.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

$(ROOT)/build/dos/bin/EBRLINK.EXE: tests/ebr_link_probe.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/bios.obj $(OBJ)/dosraw.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/ebr-link-probe.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/ebr-link-probe.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/bios.obj file $(OBJ)/dosraw.obj

.PHONY: test-ebr-link
test-ebr-link: host
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/ebr_link_probe.c disk.c partition.c bios.c dosraw.c -o $(ROOT)/build/host/ebr-link-probe
	python3 tests/test_ebr_link.py $(ROOT)/build/host/ebr-link-probe $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/test_ebr_link_faults.c disk.c partition.c -o $(ROOT)/build/host/ebr-link-faults
	$(ROOT)/build/host/ebr-link-faults
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/test_ebr_link_journal.c disk.c partition.c linkjournal.c -o $(ROOT)/build/host/ebr-link-journal-test
	$(ROOT)/build/host/ebr-link-journal-test $(ROOT)/build/host/link-test.mdj

$(ROOT)/build/dos/bin/EBRLCHK.EXE: tests/test_ebr_link_faults.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/ebr-link-faults.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/ebr-link-faults.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj

$(ROOT)/build/dos/bin/EBRLJCK.EXE: tests/test_ebr_link_journal.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/linkjournal.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/ebr-link-journal.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/ebr-link-journal.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/linkjournal.obj

$(ROOT)/build/dos/bin/LINKTXN.EXE: tests/ebr_link_txn_probe.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/linkjournal.obj $(OBJ)/bios.obj $(OBJ)/dosraw.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/ebr-link-txn-probe.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/ebr-link-txn-probe.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/linkjournal.obj file $(OBJ)/bios.obj file $(OBJ)/dosraw.obj

.PHONY: test-ebr-link-dos
test-ebr-link-dos: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE $(ROOT)/build/dos/bin/LINKTXN.EXE $(ROOT)/build/dos/bin/EBRLCHK.EXE $(ROOT)/build/dos/bin/EBRLJCK.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL'; exit 1)
	python3 tests/test_ebr_link_cli_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_ebr_link_ui_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_ebr_link_txn_dos.py $(ROOT)/build/dos/bin/LINKTXN.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_ebr_link_faults_dos.py $(ROOT)/build/dos/bin/EBRLCHK.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_ebr_link_journal_dos.py $(ROOT)/build/dos/bin/EBRLJCK.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-container
test-container: host
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/container_probe.c disk.c partition.c bios.c dosraw.c -o $(ROOT)/build/host/container-probe
	python3 tests/test_container.py $(ROOT)/build/host/container-probe $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/test_container_faults.c disk.c partition.c -o $(ROOT)/build/host/container-faults
	$(ROOT)/build/host/container-faults
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/test_container_journal.c disk.c partition.c containerjournal.c -o $(ROOT)/build/host/container-journal
	$(ROOT)/build/host/container-journal $(ROOT)/build/host/container.mdj

$(ROOT)/build/dos/bin/CONTPRV.EXE: tests/container_probe.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/bios.obj $(OBJ)/dosraw.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/container-probe.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/container-probe.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/bios.obj file $(OBJ)/dosraw.obj

$(ROOT)/build/dos/bin/CONTCHK.EXE: tests/test_container_faults.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/container-faults.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/container-faults.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj

$(ROOT)/build/dos/bin/CONTRCK.EXE: tests/test_container_journal.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/containerjournal.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/container-journal-test.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/container-journal-test.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/containerjournal.obj

$(ROOT)/build/dos/bin/CONTXN.EXE: tests/container_txn_probe.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/containerjournal.obj $(OBJ)/bios.obj $(OBJ)/dosraw.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/container-txn.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/container-txn.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/containerjournal.obj file $(OBJ)/bios.obj file $(OBJ)/dosraw.obj

.PHONY: test-container-dos
test-container-dos: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE $(ROOT)/build/dos/bin/CONTXN.EXE $(ROOT)/build/dos/bin/CONTRCK.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL'; exit 1)
	python3 tests/test_container_cli_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_container_ui_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_container_txn_dos.py $(ROOT)/build/dos/bin/CONTXN.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_container_journal_dos.py $(ROOT)/build/dos/bin/CONTRCK.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"

.PHONY: test-record
test-record: host
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/record_probe.c disk.c partition.c bios.c dosraw.c -o $(ROOT)/build/host/record-probe
	python3 tests/test_record.py $(ROOT)/build/host/record-probe $(ROOT)/build/host/magicdisk
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/test_record_faults.c disk.c partition.c -o $(ROOT)/build/host/record-faults
	$(ROOT)/build/host/record-faults
	$(CC) -std=c99 -Wall -Wextra -Werror -I. tests/test_record_journal.c disk.c partition.c recordjournal.c -o $(ROOT)/build/host/record-journal
	$(ROOT)/build/host/record-journal $(ROOT)/build/host/record.mdj

$(ROOT)/build/dos/bin/RECPRV.EXE: tests/record_probe.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/bios.obj $(OBJ)/dosraw.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/record-probe.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/record-probe.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/bios.obj file $(OBJ)/dosraw.obj

$(ROOT)/build/dos/bin/RECCHK.EXE: tests/test_record_faults.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/record-faults.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/record-faults.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj

$(ROOT)/build/dos/bin/RECRCK.EXE: tests/test_record_journal.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/recordjournal.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/record-journal-test.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/record-journal-test.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/recordjournal.obj

$(ROOT)/build/dos/bin/RECTXN.EXE: tests/record_txn_probe.c disk.h $(OBJ)/disk.obj $(OBJ)/partition.obj $(OBJ)/recordjournal.obj $(OBJ)/bios.obj $(OBJ)/dosraw.obj | $(OBJ) $(dir $(OUT))
	$(WCC) -zq -bt=dos -mf -3r -os -w4 -i=. -i=$(WATCOM)/h -fo=$(OBJ)/record-txn.obj $<
	$(WLINK) option quiet format os2 le option stub=$(WATCOM)/binw/dos32a.exe libpath $(WATCOM)/lib386 libpath $(WATCOM)/lib386/dos name $@ option stack=64k file $(OBJ)/record-txn.obj file $(OBJ)/disk.obj file $(OBJ)/partition.obj file $(OBJ)/recordjournal.obj file $(OBJ)/bios.obj file $(OBJ)/dosraw.obj

.PHONY: test-record-dos
test-record-dos: dos $(ROOT)/build/dos/bin/KEYPLAY.EXE $(ROOT)/build/dos/bin/RECTXN.EXE $(ROOT)/build/dos/bin/RECRCK.EXE
	@test -n "$(DOS_BOOT)" -a -n "$(DOS_KERNEL)" || (echo 'Set DOS_BOOT and DOS_KERNEL'; exit 1)
	python3 tests/test_record_cli_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_record_ui_dos.py $(OUT) "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_record_txn_dos.py $(ROOT)/build/dos/bin/RECTXN.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
	python3 tests/test_record_journal_dos.py $(ROOT)/build/dos/bin/RECRCK.EXE "$(DOS_BOOT)" --kernel "$(DOS_KERNEL)"
