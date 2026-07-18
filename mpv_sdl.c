// player_mpv.c - Intel macOS 独立窗口版（libmpv 硬解）
// 编译命令：
// gcc player_mpv.c -o player $(pkg-config --cflags --libs mpv sdl2) -lpthread -lm -O2

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpv/client.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <video_file>\n", argv[0]);
        return 1;
    }

    mpv_handle *ctx = mpv_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create mpv context\n");
        return 1;
    }

    // 🚀 核心配置：Intel Mac 硬解 + 强制窗口 + 高性能管线
    mpv_set_option_string(ctx, "hwdec", "videotoolbox");
    mpv_set_option_string(ctx, "vo", "gpu");
    mpv_set_option_string(ctx, "gpu-context", "mac");
    mpv_set_option_string(ctx, "force-window", "yes");      // 👈 关键：创建独立窗口
    mpv_set_option_string(ctx, "geometry", "1280x720");   
    mpv_set_option_string(ctx, "cache", "yes");
    mpv_set_option_string(ctx, "cache-secs", "10");
    mpv_set_option_string(ctx, "keep-open", "yes");
    mpv_set_option_string(ctx, "title", "APlayer");

    if (mpv_initialize(ctx) < 0) {
        fprintf(stderr, "Failed to initialize mpv\n");
        mpv_free(ctx);
        return 1;
    }

    fprintf(stderr, "[INFO] mpv initialized. Loading: %s\n", argv[1]);
    const char *cmd[] = {"loadfile", argv[1], NULL};
    mpv_command(ctx, cmd);

    fprintf(stderr, "[INFO] Playing. Close window to quit.\n");
    
    while (1) {
        mpv_event *event = mpv_wait_event(ctx, -1.0);
        if (event->event_id == MPV_EVENT_SHUTDOWN) break;
        if (event->event_id == MPV_EVENT_END_FILE) {
            fprintf(stderr, "[INFO] Playback finished.\n");
            break;
        }
    }

    mpv_free(ctx);
    return 0;
}