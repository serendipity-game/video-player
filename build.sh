clang av1.c -o myplayer \
    $(pkg-config --cflags --libs libavformat libavcodec libswscale libswresample libavutil sdl2) \
    -lpthread -lm -O2