ifeq ($(OS),Windows_NT)
	BUILD_CMD = gcc -g map_viewer.c -I"external/include" -L"external/lib" -lmingw32 -lSDL2main -lSDL2 -lSDL2_image -lcurl -Wall -o map_viewer.exe
    RUN_CMD = build/map_viewer.exe
else
	BUILD_CMD = gcc -g map_viewer.c `sdl2-config --cflags --libs` -lSDL2_image -lcurl -lm -Wall -o map_viewer
    RUN_CMD = ./map_viewer
endif

all:
	$(BUILD_CMD)	

run:
	$(RUN_CMD)

clean:
	rm -rf map_viewer *.exe build/*.exe
