CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g

# One binary per source file. Add more names here (e.g. supervisor_epoll)
# and `make` will build <name> from <name>.cpp.
PROGS := supervisor

.PHONY: all clean run

all: $(PROGS)

%: %.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

# Rebuild if needed, then run: `make run` or `make run N=4`
N ?= 4
run: supervisor
	./supervisor $(N)

clean:
	rm -f $(PROGS)
