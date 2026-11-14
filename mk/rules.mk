# Builds one module (a program under src/ or a library under lib/) for one
# platform:   make -f mk/rules.mk DIR=src/hello PLATFORM=dos
#
# The module is described by $(DIR)/project.mk; see README.md for the
# variables it may set.

.SUFFIXES:
.DELETE_ON_ERROR:

ifeq ($(and $(DIR),$(PLATFORM)),)
$(error usage: make -f mk/rules.mk DIR=<module dir> PLATFORM=<platform>)
endif
ifeq ($(wildcard mk/platform/$(PLATFORM).mk),)
$(error unknown platform '$(PLATFORM)')
endif

# ---- module description (defaults, then project.mk) -----------------------
NAME      := $(notdir $(DIR))
KIND      := exe
TYPE      := console
PLATFORMS := dos win16 win9x winnt
SRCS      :=
RES       :=
LIBS      := retro
UNICODE   :=
include $(DIR)/project.mk

.PHONY: all
ifeq ($(filter $(PLATFORM),$(PLATFORMS)),)
all: ; @:
else

ifeq ($(filter $(KIND),exe lib),)
$(error $(DIR): KIND must be 'exe' or 'lib', not '$(KIND)')
endif
ifeq ($(filter $(TYPE),console gui),)
$(error $(DIR): TYPE must be 'console' or 'gui', not '$(TYPE)')
endif
ifneq ($(shell printf '%s' '$(NAME)' | grep -Eq '^[A-Za-z0-9_-]{1,8}$$' || echo bad),)
$(error $(DIR): NAME '$(NAME)' must be a valid 8.3 base name)
endif

# ---- platform --------------------------------------------------------------
include mk/platform/$(PLATFORM).mk

# A library is built once per memory model; the model is passed on the command
# line by the program that links it (and thereby overrides this assignment).
MODEL := $(or $(MODEL_$(PLATFORM)),$(DEFAULT_MODEL))
ifeq ($(filter $(MODEL),$(MODELS)),)
$(error $(DIR): memory model '$(MODEL)' is not valid for $(PLATFORM) ($(MODELS)))
endif
CPU := $(or $(CPU_$(PLATFORM)),$(DEFAULT_CPU))
# Tiny model (.COM) is small-model code linked as a .COM file.
CC_MODEL := $(if $(filter t,$(MODEL)),s,$(MODEL))

ifeq ($(strip $(SRCS) $(SRCS_$(PLATFORM))),)
SRCS := $(notdir $(wildcard $(DIR)/*.c $(DIR)/*.cpp $(DIR)/*.asm))
else
SRCS := $(SRCS) $(SRCS_$(PLATFORM))
endif
ifeq ($(HAS_RES),)
RES :=
endif

BUILD   := build/$(PLATFORM)
BIN_DIR := $(BUILD)/bin
LIB_DIR := $(BUILD)/lib/$(MODEL)
ifeq ($(KIND),lib)
OBJ_DIR := $(BUILD)/obj/lib-$(NAME)-$(MODEL)
TARGET  := $(LIB_DIR)/$(NAME).lib
else
OBJ_DIR := $(BUILD)/obj/$(NAME)
TARGET  := $(BIN_DIR)/$(shell echo $(NAME) | tr a-z A-Z).$(call exe_ext,$(MODEL))
endif

OBJS      := $(addprefix $(OBJ_DIR)/,$(addsuffix .obj,$(basename $(SRCS))))
RES_FILE  := $(if $(RES),$(OBJ_DIR)/$(basename $(RES)).res)
LIB_FILES := $(foreach l,$(LIBS),$(LIB_DIR)/$(l).lib)
LIB_INCS  := $(foreach l,$(LIBS),-i=lib/$(l))

DEFS := -DRETRO_$(PLATFORM_ID) -DRETRO_BITS=$(BITS) \
        $(if $(filter gui,$(TYPE)),-DRETRO_GUI,-DRETRO_CONSOLE) \
        $(if $(and $(UNICODE),$(HAS_UNICODE)),-DUNICODE -D_UNICODE)

COMMON  := -zq -m$(CC_MODEL) -$(CPU)$(CPU_SUFFIX) $(PLAT_CFLAGS) $(PLAT_CFLAGS_$(TYPE)) \
           $(PLAT_INCS) -i=$(DIR) $(LIB_INCS) $(DEFS)
CFLAGS_ALL   := $(COMMON) -w4 -os $(CFLAGS) $(CFLAGS_$(PLATFORM))
CXXFLAGS_ALL := $(COMMON) -w4 -os -xs $(CXXFLAGS) $(CXXFLAGS_$(PLATFORM))
AFLAGS_ALL   := -zq -m$(CC_MODEL) -$(CPU) $(PLAT_AFLAGS) -i=$(DIR) $(DEFS) $(AFLAGS) $(AFLAGS_$(PLATFORM))
RCFLAGS_ALL  := -q -r $(PLAT_RCFLAGS) -i=$(DIR) $(DEFS) $(RCFLAGS) $(RCFLAGS_$(PLATFORM))
LDFLAGS_ALL  := $(call link_sys,$(TYPE),$(MODEL)) $(LDFLAGS) $(LDFLAGS_$(PLATFORM))

ifeq ($(V),1)
Q :=
else
Q := @
endif
say = @printf '  %-5s %-6s %s\n' '$(1)' '$(PLATFORM)' '$(2)'

all: $(TARGET) ; @:

$(OBJ_DIR) $(BIN_DIR) $(LIB_DIR):
	$(Q)mkdir -p $@

# The compilers list the main source without its directory in the dependency
# file; drop it (the pattern rule already depends on the source).
DEPFLAGS = -ad=$(@:.obj=.d) -adt=$@ -adfs
fixdeps  = sed -i 's| : [^ ]*| :|' $(@:.obj=.d)

$(OBJ_DIR)/%.obj: $(DIR)/%.c | $(OBJ_DIR)
	$(call say,CC,$<)
	$(Q)$(CC) $(CFLAGS_ALL) $(DEPFLAGS) -fo=$@ $< && $(fixdeps)

$(OBJ_DIR)/%.obj: $(DIR)/%.cpp | $(OBJ_DIR)
	$(call say,CXX,$<)
	$(Q)$(CXX) $(CXXFLAGS_ALL) $(DEPFLAGS) -fo=$@ $< && $(fixdeps)

$(OBJ_DIR)/%.obj: $(DIR)/%.asm | $(OBJ_DIR)
	$(call say,ASM,$<)
	$(Q)wasm $(AFLAGS_ALL) -fo=$@ $<

$(OBJ_DIR)/%.res: $(DIR)/%.rc | $(OBJ_DIR)
	$(call say,RC,$<)
	$(Q)wrc $(RCFLAGS_ALL) -fo=$@ $<

# Resources also depend on the project's headers and on the files in res/.
$(RES_FILE): $(wildcard $(DIR)/*.h $(DIR)/res/*)

# Everything is rebuilt when the build system or the module description changes.
$(OBJS) $(RES_FILE): $(DIR)/project.mk mk/rules.mk mk/platform/$(PLATFORM).mk

ifeq ($(KIND),lib)

$(TARGET): $(OBJS) | $(LIB_DIR)
	$(call say,LIB,$@)
	$(Q)rm -f $@
	$(Q)wlib -q -n -b $@ $(addprefix +,$(OBJS))

else

# Libraries are (re)built by a sub-make in the model this program uses; the
# program is only relinked when a library file actually changed.
.PHONY: libs
libs:
	+$(Q)for l in $(LIBS); do \
	  $(MAKE) --no-print-directory -f mk/rules.mk DIR=lib/$$l PLATFORM=$(PLATFORM) MODEL=$(MODEL) || exit 1; \
	done
$(LIB_FILES): libs ;

$(TARGET): $(OBJS) $(RES_FILE) $(LIB_FILES) | $(BIN_DIR)
	$(call say,LINK,$@)
	$(Q)wlink option quiet $(LDFLAGS_ALL) name $@ \
	  option map=$(OBJ_DIR)/$(NAME).map \
	  $(if $(RES_FILE),option resource=$(RES_FILE)) \
	  $(addprefix file ,$(OBJS)) $(addprefix library ,$(LIB_FILES))

endif

-include $(OBJS:.obj=.d)

endif
