qt?=5
dynarec?=
CMAKE_FLAGS?=
CMAKE_EXTRA:=

ifeq ($(qt),6)
	CMAKE_EXTRA += -DUSE_QT6=ON
endif

ifeq ($(dynarec),new)
	CMAKE_EXTRA += -DNEW_DYNAREC=ON
endif

define do_configure
	cmake --preset $(1) $(CMAKE_EXTRA) $(CMAKE_FLAGS)
endef

define do_build
	$(call do_configure,$(1))
	cmake --build build/$(1)
endef

.PHONY: all debug development dev_debug optimized regular clean \
	new_regular new_debug new_dev_debug

all: regular

regular:
	$(call do_build,regular)

optimized:
	$(call do_build,optimized)

debug:
	$(call do_build,debug)

dev_debug:
	$(call do_build,dev_debug)

development:
	$(call do_build,development)

new_regular:
	$(call do_build,new_dynarec)

new_debug:
	$(call do_build,new_dynarec_debug)

new_dev_debug:
	$(call do_build,new_dynarec_dev_debug)

clean:
	$(RM) -r build
