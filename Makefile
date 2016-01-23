OB_CXXFLAGS = -g -Wall -Wextra -O2

CUBESCRIPT_PATH = ../libcubescript
OCTASTD_PATH = ../octastd

FILES = main.o globs.o cubescript.o

OB_CXXFLAGS += -std=c++14 -I. -I$(CUBESCRIPT_PATH) -I$(OCTASTD_PATH) -pthread

all: obuild

obuild: $(FILES)
	$(CXX) $(CXXFLAGS) $(OB_CXXFLAGS) $(LDFLAGS) -lstdthreads -o obuild $(FILES)

.cc.o:
	$(CXX) $(CXXFLAGS) $(OB_CXXFLAGS) -c -o $@ $<

cubescript.o: $(CUBESCRIPT_PATH)/cubescript.cc
	$(CXX) $(CXXFLAGS) $(OB_CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(FILES) obuild

main.o: globs.hh
main.o: $(CUBESCRIPT_PATH)/cubescript.hh

globs.o: globs.hh
globs.o: $(CUBESCRIPT_PATH)/cubescript.hh

cubescript.o: $(CUBESCRIPT_PATH)/cubescript.hh