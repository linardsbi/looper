.POSIX:

DEPENDENCIES = sdl2 SDL2_mixer

CXX = clang++
CXXFLAGS = -std=c++2b -Os -march=native -Wall -Wextra -Wpedantic -Wconversion -flto
DEBUGFLAGS = -std=c++2b -fsanitize=address,undefined -g
CFLAGS = `pkg-config --cflags ${DEPENDENCIES}`
LIBS = `pkg-config --libs ${DEPENDENCIES}`

debug: 
	$(CXX) $(CFLAGS) ${DEBUGFLAGS} ${LIBS} src/main.cpp -o looper
looper:
	$(CXX) $(CFLAGS) ${CXXFLAGS} ${LIBS} src/main.cpp -o looper
clean:
	rm -f *.o *.gch looper
all: looper

.PHONY: clean
