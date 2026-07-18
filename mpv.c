// player_mpv.c
// 编译：gcc player_mpv.c -o player $(pkg-config --cflags --libs mpv sdl2) -lpthread -lm -O2
#include <mpv/client.h>
#include <SDL2/SDL.h>
#include <stdio.h>

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <file>\n", argv[0]); return 1; }

    // 激活 macOS GUI 子系统（必须，否则 mpv 窗口不绘制）
    SDL_Init(SDL_INIT_VIDEO);

    mpv_handle *ctx = mpv_create();
    mpv_set_option_string(ctx, "hwdec", "videotoolbox");
    mpv_set_option_string(ctx, "vo", "gpu");
    mpv_set_option_string(ctx, "force-window", "yes");
    mpv_set_option_string(ctx, "fs", "yes");
    mpv_set_option_string(ctx, "keep-open", "yes");
    mpv_set_option_string(ctx, "input-terminal", "no");

    if (mpv_initialize(ctx) < 0) {
        fprintf(stderr, "mpv init failed\n");
        return 1;
    }

    const char *cmd[] = {"loadfile", argv[1], NULL};
    mpv_command(ctx, cmd);

    // 主循环：mpv 事件 + macOS 窗口泵送
    while (1) {
        mpv_event *ev = mpv_wait_event(ctx, 0.05);
        SDL_PumpEvents();  // 👈 关键：让 macOS Cocoa 刷新窗口
        if (ev->event_id == MPV_EVENT_SHUTDOWN) break;
        if (ev->event_id == MPV_EVENT_SHUTDOWN || ev->event_id == MPV_EVENT_END_FILE) break;
    }

    mpv_free(ctx);
    SDL_Quit();
    return 0;
}