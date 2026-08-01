MODULE_big = qos

# Support for pg_buildext build directories
ifdef VPATH
OBJS = $(VPATH)/src/qos.o $(VPATH)/src/hooks.o $(VPATH)/src/hooks_cache.o \
       $(VPATH)/src/hooks_statement.o $(VPATH)/src/hooks_transaction.o \
       $(VPATH)/src/hooks_resource.o $(VPATH)/src/hooks_rate.o \
       $(VPATH)/src/qos_stats.o $(VPATH)/src/qos_shmem_slot.o
else
OBJS = src/qos.o src/hooks.o src/hooks_cache.o src/hooks_statement.o \
       src/hooks_transaction.o src/hooks_resource.o src/hooks_rate.o \
       src/qos_stats.o src/qos_shmem_slot.o
endif

EXTENSION = qos
DATA = qos--1.0.sql qos--1.0--1.1.sql qos--1.1.sql

PGXS := $(shell $(PG_CONFIG) --pgxs)

include $(PGXS)