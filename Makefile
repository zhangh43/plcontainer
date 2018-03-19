#------------------------------------------------------------------------------
# 
# Copyright (c) 2016-Present Pivotal Software, Inc
#
#------------------------------------------------------------------------------
MODULE_big = plcontainer
# for extension
EXTENSION = plcontainer

# Directories
SRCDIR = ./src
COMMONDIR = ./src/common
MGMTDIR = ./management
PYCLIENTDIR = src/pyclient/bin
RCLIENTDIR = src/rclient/bin

# the DATA is including files for extension use
DATA = $(MGMTDIR)/sql/plcontainer--1.0.0.sql $(MGMTDIR)/sql/plcontainer--unpackaged--1.0.0.sql plcontainer.control
# DATA_built will built sql from .in file
DATA_built = $(MGMTDIR)/sql/plcontainer_install.sql $(MGMTDIR)/sql/plcontainer_uninstall.sql

# Files to build
FILES = $(shell find $(SRCDIR) -not -path "*client*" -type f -name "*.c")
OBJS = $(foreach FILE,$(FILES),$(subst .c,.o,$(FILE)))

PGXS := $(shell pg_config --pgxs)
include $(PGXS)

# FIXME: We might need a configure script to handle below checks later.
# See https://github.com/greenplum-db/plcontainer/issues/322

# Plcontainer version
PLCONTAINER_VERSION = "1.3.0-3-gd7ba1b0"
ifeq ($(PLCONTAINER_VERSION),)
  $(error can not determine the plcontainer version)
else
  ver = \#define PLCONTAINER_VERSION \"$(PLCONTAINER_VERSION)\"
  $(shell if [ ! -f src/common/config.h ] || [ "$(ver)" != "`cat src/common/config.h`" ]; then echo "$(ver)" > src/common/config.h; fi)
endif

# Curl
CURL_CONFIG = $(shell type -p curl-config || echo no)
ifneq ($(CURL_CONFIG),no)
  CURL_VERSION  = $(shell $(CURL_CONFIG) --version | cut -d" " -f2)
  VERSION_CHECK = $(shell expr $(CURL_VERSION) \>\= 7.40.0)
ifeq ($(VERSION_CHECK),1)
  override CFLAGS += $(shell $(CURL_CONFIG) --cflags)
  SHLIB_LINK = $(shell $(CURL_CONFIG) --libs)
else
  $(error curl version is < 7.40, please update your libcurl installation.)
endif
else
  $(error curl-config is not found, please check libcurl installation.)
endif

#libxml
LIBXML_CONFIG = $(shell which xml2-config || echo no)
ifneq ($(LIBXML_CONFIG),no)
  override CFLAGS += $(shell xml2-config --cflags)
  override SHLIB_LINK += $(shell xml2-config --libs)
else
  $(error xml2-config is missing. Have you installed libxml? On RHEL/CENTOS you could install by runnning "yum install libxml2-devel" )
endif

#json-c
LIBJSON = $(shell pkg-config --libs json-c || echo no)
ifneq ($(LIBJSON),no)
  override SHLIB_LINK += $(shell pkg-config --libs json-c)
  override CFLAGS += $(shell pkg-config --cflags json-c)
else
  $(error libjson-c.so is missing. Have you installed json-c? On RHEL/CENTOS you could install by running "yum install json-c-devel" )
endif

PLCONTAINERDIR = $(DESTDIR)$(datadir)/plcontainer

override CFLAGS += -Werror -Wextra -Wall

ifeq ($(ENABLE_COVERAGE),yes)
  override CFLAGS += -coverage
  override SHLIB_LINK += -lgcov --coverage
  override CLIENT_CFLAGS += -coverage -O0 -g
  override CLIENT_LDFLAGS += -lgcov --coverage
else
  override CLIENT_CFLAGS += -O3 -g
endif

# detected the docker API version, only for centos 6
RHEL_MAJOR_OS=$(shell cat /etc/redhat-release | sed s/.*release\ // | sed s/\ .*// | awk -F '.' '{print $$1}' )
ifeq ($(RHEL_MAJOR_OS), 6)
  override CFLAGS +=  -DDOCKER_API_LOW
endif
all: all-lib
	@echo "Build PL/Container Done."

install: all installdirs install-lib install-extra

clean: clean-clients clean-coverage
distclean: distclean-config

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(bindir)'
	$(MKDIR_P) '$(PLCONTAINERDIR)'

uninstall: uninstall-lib
	rm -f '$(DESTDIR)$(bindir)/plcontainer'
	rm -rf '$(PLCONTAINERDIR)'

.PHONY: install-extra
install-extra: installdirs
	$(INSTALL_PROGRAM) '$(MGMTDIR)/bin/plcontainer'                         '$(DESTDIR)$(bindir)/plcontainer'
	$(INSTALL_DATA)    '$(MGMTDIR)/config/plcontainer_configuration.xml'    '$(PLCONTAINERDIR)/'
	$(INSTALL_DATA)    '$(MGMTDIR)/sql/plcontainer_install.sql'             '$(PLCONTAINERDIR)/'
	$(INSTALL_DATA)    '$(MGMTDIR)/sql/plcontainer_uninstall.sql'           '$(PLCONTAINERDIR)/'

.PHONY: install-clients
install-clients: build-clients
	$(MKDIR_P) '$(DESTDIR)$(bindir)/plcontainer_clients'
	cp $(PYCLIENTDIR)/* $(DESTDIR)$(bindir)/plcontainer_clients/
	cp $(RCLIENTDIR)/*  $(DESTDIR)$(bindir)/plcontainer_clients/

.PHONY: installcheck
installcheck:
	$(MAKE) -C tests tests

.PHONY: build-clients
build-clients:
	CC='$(CC)' CFLAGS='$(CLIENT_CFLAGS)' LDFLAGS='$(CLIENT_LDFLAGS)' $(MAKE) -C $(SRCDIR)/pyclient all
	CC='$(CC)' CFLAGS='$(CLIENT_CFLAGS)' LDFLAGS='$(CLIENT_LDFLAGS)' $(MAKE) -C $(SRCDIR)/rclient all

.PHONY: clean-clients
clean-clients:
	$(MAKE) -C $(SRCDIR)/pyclient clean
	$(MAKE) -C $(SRCDIR)/rclient clean

.PHONY: clean-coverage
clean-coverage:
	rm -f `find . -name '*.gcda' -print`
	rm -f `find . -name '*.gcno' -print`
	rm -rf coverage.info coverage_result

.PHONY: distclean-config
distclean-config:
	rm -f src/common/config.h

.PHONY: coverage-report
coverage-report:
	lcov -c -o coverage.info -d .
	genhtml coverage.info -o coverage_result
