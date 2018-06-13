CXX = g++

ifdef DEBUG
# CXXFLAGS for debug
CXXFLAGS := -ggdb -Wall -fPIC -std=c++14 -O0 -pthread -D_GLIBCXX_DEBUG -D_GLIBCXX_DEBUG_PEDANTIC 
else
# CXXFLAGS for optimized
CXXFLAGS := -Wall -fPIC -std=c++14 -O2 -pthread
endif

ROOTFLAGS := `root-config --cflags --glibs --libs`
ZEROMQ_INC := `pkg-config --cflags libzmq`
ZEROMQ_LIB := `pkg-config --libs libzmq`
CZMQ_INC := `pkg-config --cflags libczmq`
CZMQ_LIB := `pkg-config --libs libczmq`
INCLUDE += $(ZEROMQ_INC) $(CZMQ_INC) -I$(TRACE_INC)
LDLIBS += $(ZEROMQ_LIB) $(CZMQ_LIB) -lrt

# Add any more executables you want here. They should have a single
# .cxx file with the same name as the executable you want
BINARIES := primsim
OBJS := $(addsuffix .o,$(BINARIES))
DEPS := $(addsuffix .d,$(BINARIES))
SRCS := $(addsuffix .cxx,$(BINARIES))

all : $(BINARIES)

.PHONY: all

%.o: %.cxx
	$(CXX) $(INCLUDE) $(CXXFLAGS) -MMD -MP $(ROOTFLAGS) -o $@ -c $<

# All of the binaries have the same format, so use a "static pattern
# rule". Each binary "foo" depends on "foo.o" and we build it with the
# recipe given ($@ will be the name of the binary)
$(BINARIES) : %: %.o
	$(CXX) $(INCLUDE) $(CXXFLAGS) $(ROOTFLAGS) $(LDLIBS) -o $@ $^

clean:
	rm -f $(BINARIES) *.o *.d

.PHONY: clean

-include $(DEPS)