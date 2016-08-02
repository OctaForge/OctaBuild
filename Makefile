OB_CXXFLAGS = -g -Wall -Wextra -Wshadow -Wold-style-cast -O2

CUBESCRIPT_PATH = ../libcubescript
OCTASTD_PATH = ../octastd

FILES = main.o globs.o

OB_CXXFLAGS += -std=c++14 -I. -I$(CUBESCRIPT_PATH) -I$(OCTASTD_PATH) -pthread

all: obuild

obuild: $(FILES)
	$(CXX) $(CXXFLAGS) $(OB_CXXFLAGS) $(LDFLAGS) -o obuild $(FILES) \
	$(CUBESCRIPT_PATH)/libcubescript.a

.cc.o:
	$(CXX) $(CXXFLAGS) $(OB_CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(FILES) obuild

main.o: tpool.hh $(CUBESCRIPT_PATH)/cubescript.hh
globs.o: $(CUBESCRIPT_PATH)/cubescript.hh
