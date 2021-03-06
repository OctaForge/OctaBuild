OB_CXXFLAGS = -g -Wall -Wextra -Wshadow -Wold-style-cast -O2

CUBESCRIPT_PATH = ../libcubescript
OSTD_PATH = ../libostd

FILES = main.o

OB_CXXFLAGS += -std=c++1z -I. -I$(CUBESCRIPT_PATH)/include -I$(OSTD_PATH) -pthread

all: obuild

obuild: $(FILES)
	$(CXX) $(CXXFLAGS) $(OB_CXXFLAGS) -o obuild $(FILES) \
	$(CUBESCRIPT_PATH)/libcubescript.a $(OSTD_PATH)/libostd.a $(LDFLAGS)

.cc.o:
	$(CXX) $(CXXFLAGS) $(OB_CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(FILES) obuild

main.o: $(CUBESCRIPT_PATH)/include/cubescript/cubescript.hh
