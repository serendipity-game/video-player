clang libv.c -o myplayer \
  $(pkg-config --cflags --libs libmpv sdl2) -lpthread