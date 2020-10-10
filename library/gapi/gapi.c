#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>

#include <gapi.h>
#include <gscreen.h>
#include <glayer.h>
#include <gfont.h>

/* inner function */
extern int g_del_bitmap_all();
extern int g_del_timer_all();
extern int g_del_window_all();
extern int g_del_layer_all();

int g_init(void)
{
    /* call ginit */
    int val = syscall0(int, SYS_GINIT);
    if (val < 0) 
        return -1;
    /* get screen info */
    if (g_get_screen(&_g_screen) < 0)
        return -1;
    
    if (g_init_font() < 0)
        return -1;

    return val;
}

int g_quit(void)
{
    
    /* 关闭定时器 */
    if (g_del_timer_all() < 0)
        return 0;
        
    /* 先关闭窗口 */
    if (g_del_window_all() < 0)
        return -1;
    
    g_del_bitmap_all(); // 删除位图资源

    /* 再关闭图层 */
    if (g_del_layer_all() < 0)
        return -1;

    return syscall0(int, SYS_GQUIT);
}
