OB_CXXFLAGS = -g -Wall -Wextra -Wshadow -Wold-style-cast -O2

CUBESCRIPT_PATH = ../libcubescript
OCTASTD_PATH = ../octastd

FILES = main.o

OB_CXXFLAGS += -std=c++1z -I. -I$(CUBESCRIPT_PATH)/include -I$(OCTASTD_PATH) -pthread

all: obuild

obuild: $(FILES)
	$(CXX) $(CXXFLAGS) $(OB_CXXFLAGS) -o obuild $(FILES) \
	$(CUBESCRIPT_PATH)/libcubescript.a $(OCTASTD_PATH)/libostd.a $(LDFLAGS)

.cc.o:
	$(CXX) $(CXXFLAGS) $(OB_CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(FILES) obuild

main.o: tpool.hh $(CUBESCRIPT_PATH)/include/cubescript/cubescript.hh
