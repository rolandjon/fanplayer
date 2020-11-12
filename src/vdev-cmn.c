// 包含头文件
#include "vdev.h"
#include "libavutil/log.h"
#include "libavutil/time.h"

// 内部常量定义
#define COMPLETED_COUNTER  10

// 函数实现
void* vdev_create(int type, void *surface, int bufnum, int w, int h, int ftime, CMNVARS *cmnvars)
{
    VDEV_COMMON_CTXT *c = NULL;
#ifdef WIN32
    switch (type) {
    case VDEV_RENDER_TYPE_GDI: c = (VDEV_COMMON_CTXT*)vdev_gdi_create(surface, bufnum); break;
    case VDEV_RENDER_TYPE_D3D: c = (VDEV_COMMON_CTXT*)vdev_d3d_create(surface, bufnum); break;
    }
    if (1) {
        BITMAPINFO bmpinfo = {0};
        HDC        hdc     = NULL;
        hdc = GetDC((HWND)c->surface);
        c->hoverlay = CreateCompatibleDC(hdc);
        bmpinfo.bmiHeader.biSize        =  sizeof(BITMAPINFOHEADER);
        bmpinfo.bmiHeader.biWidth       =  GetSystemMetrics(SM_CXSCREEN);
        bmpinfo.bmiHeader.biHeight      = -GetSystemMetrics(SM_CYSCREEN);
        bmpinfo.bmiHeader.biPlanes      =  1;
        bmpinfo.bmiHeader.biBitCount    =  32;
        bmpinfo.bmiHeader.biCompression =  BI_RGB;
        c->hoverbmp = CreateDIBSection(c->hoverlay, &bmpinfo, DIB_RGB_COLORS, &c->poverlay, NULL, 0);
        SelectObject(c->hoverlay, c->hoverbmp);
        ReleaseDC((HWND)c->surface, hdc);
    }
    if (!c) return NULL;
#endif
#ifdef ANDROID
    c = (VDEV_COMMON_CTXT*)vdev_android_create(surface, bufnum);
    if (!c) return NULL;
    c->tickavdiff=-ftime * 2; // 2 should equals to (DEF_ADEV_BUF_NUM - 1)
#endif
    c->surface     = surface;
    c->vw          = w;
    c->vh          = h;
    c->rectr.right = MAX(w, 1);
    c->rectr.bottom= MAX(h, 1);
    c->rectv.right = MAX(w, 1);
    c->rectv.bottom= MAX(h, 1);
    c->tickframe   = ftime;
    c->ticksleep   = ftime;
    c->cmnvars     = cmnvars;
    return c;
}

void vdev_destroy(void *ctxt)
{
    VDEV_COMMON_CTXT *c = (VDEV_COMMON_CTXT*)ctxt;

    //++ rendering thread safely exit
    if (c->thread) {
        pthread_mutex_lock(&c->mutex);
        c->status = VDEV_CLOSE;
        pthread_cond_signal(&c->cond);
        pthread_mutex_unlock(&c->mutex);
        pthread_join(c->thread, NULL);
    }
    //-- rendering thread safely exit

#ifdef WIN32
    DeleteDC(c->hoverlay);
    DeleteObject(c->hoverbmp);
#endif
    if (c->destroy) c->destroy(c);
}

void vdev_lock(void *ctxt, uint8_t *buffer[8], int linesize[8], int64_t pts)
{
    VDEV_COMMON_CTXT *c = (VDEV_COMMON_CTXT*)ctxt;
    if (c->lock) c->lock(c, buffer, linesize, pts);
}

void vdev_unlock(void *ctxt)
{
    VDEV_COMMON_CTXT *c = (VDEV_COMMON_CTXT*)ctxt;
    if (c->unlock) c->unlock(c);
}

void vdev_setrect(void *ctxt, int x, int y, int w, int h)
{
    VDEV_COMMON_CTXT *c = (VDEV_COMMON_CTXT*)ctxt;
    pthread_mutex_lock(&c->mutex);
    c->rectr.left  = x;     c->rectr.top    = y;
    c->rectr.right = x + w; c->rectr.bottom = y + h;
    pthread_mutex_unlock(&c->mutex);
    vdev_setparam(c, PARAM_VIDEO_MODE, &c->vm);
    if (c->setrect) c->setrect(c, x, y, w, h);
}

void vdev_pause(void *ctxt, int pause)
{
    VDEV_COMMON_CTXT *c = (VDEV_COMMON_CTXT*)ctxt;
    if (!ctxt) return;
    if (pause) c->status |=  VDEV_PAUSE;
    else       c->status &= ~VDEV_PAUSE;
}

void vdev_reset(void *ctxt)
{
    VDEV_COMMON_CTXT *c = (VDEV_COMMON_CTXT*)ctxt;
    if (!ctxt) return;
}

void vdev_setparam(void *ctxt, int id, void *param)
{
    VDEV_COMMON_CTXT *c = (VDEV_COMMON_CTXT*)ctxt;
    if (!ctxt) return;
    switch (id) {
    case PARAM_VIDEO_MODE:
        {
            int rw = c->rectr.right - c->rectr.left, rh = c->rectr.bottom - c->rectr.top, vw, vh;
            if (*(int*)param == VIDEO_MODE_LETTERBOX) {
                if (rw * c->vh < rh * c->vw) {
                    vw = rw; vh = vw * c->vh / c->vw;
                } else {
                    vh = rh; vw = vh * c->vw / c->vh;
                }
            } else { vw = rw; vh = rh; }
            pthread_mutex_lock(&c->mutex);
            c->rectv.left  = (rw - vw) / 2;
            c->rectv.top   = (rh - vh) / 2;
            c->rectv.right = c->rectv.left + vw;
            c->rectv.bottom= c->rectv.top  + vh;
            c->vm      = *(int*)param;
            c->status |= VDEV_CLEAR;
            pthread_mutex_unlock(&c->mutex);
        }
        break;
    case PARAM_PLAY_SPEED_VALUE:
        if (param) c->speed = *(int*)param;
        break;
    case PARAM_AVSYNC_TIME_DIFF:
        if (param) c->tickavdiff = *(int*)param;
        break;
#ifdef WIN32
    case PARAM_VDEV_SET_OVERLAY_RECT:
        if (param) {
            int i;
            for (i=0; i<sizeof(c->overlay_rects)/sizeof(c->overlay_rects[0]); i++) {
                c->overlay_rects[i] = ((RECTOVERLAY*)param)[i];
                if (((RECTOVERLAY*)param)[i].type == 0) break;
            }
        } else c->overlay_rects[0].type = 0;
        break;
#endif
    }
    if (c->setparam) c->setparam(c, id, param);
}

void vdev_getparam(void *ctxt, int id, void *param)
{
    VDEV_COMMON_CTXT *c = (VDEV_COMMON_CTXT*)ctxt;
    if (!ctxt || !param) return;
    switch (id) {
    case PARAM_VIDEO_MODE      : *(int*)param = c->vm;         break;
    case PARAM_PLAY_SPEED_VALUE: *(int*)param = c->speed;      break;
    case PARAM_AVSYNC_TIME_DIFF: *(int*)param = c->tickavdiff; break;
#ifdef WIN32
    case PARAM_VDEV_GET_OVERLAY_HDC:
        *(HDC*)param = c->hoverlay;
        break;
#endif
    }
    if (c->getparam) c->getparam(c, id, param);
}

void vdev_avsync_and_complete(void *ctxt)
{
    VDEV_COMMON_CTXT *c = (VDEV_COMMON_CTXT*)ctxt;
    int     tickframe, tickdiff, scdiff, avdiff = -1;
    int64_t tickcur, sysclock;

    if (!(c->status & VDEV_PAUSE)) {
        //++ play completed ++//
        if (c->completed_apts != c->cmnvars->apts || c->completed_vpts != c->cmnvars->vpts) {
            c->completed_apts = c->cmnvars->apts;
            c->completed_vpts = c->cmnvars->vpts;
            c->completed_counter = 0;
            c->status &=~VDEV_COMPLETED;
        } else if (!c->cmnvars->apktn && !c->cmnvars->apktn && ++c->completed_counter == COMPLETED_COUNTER) {
            c->status |= VDEV_COMPLETED;
            player_send_message(c->cmnvars->winmsg, MSG_PLAY_COMPLETED, 0);
        }
        //-- play completed --//

        //++ frame rate & av sync control ++//
        tickframe   = 100 * c->tickframe / c->speed;
        tickcur     = av_gettime_relative() / 1000;
        tickdiff    = (int)(tickcur - c->ticklast);
        c->ticklast = tickcur;

        sysclock= c->cmnvars->start_pts + (tickcur - c->cmnvars->start_tick) * c->speed / 100;
        scdiff  = (int)(sysclock - c->cmnvars->vpts - c->tickavdiff); // diff between system clock and video pts
        avdiff  = (int)(c->cmnvars->apts  - c->cmnvars->vpts - c->tickavdiff); // diff between audio and video pts
        avdiff  = c->cmnvars->apts <= 0 ? scdiff : avdiff; // if apts is invalid, sync video to system clock

        if (tickdiff - tickframe >  5) c->ticksleep--;
        if (tickdiff - tickframe < -5) c->ticksleep++;
        if (c->cmnvars->vpts >= 0) {
            if      (avdiff >  500) c->ticksleep -= 3;
            else if (avdiff >  50 ) c->ticksleep -= 2;
            else if (avdiff >  30 ) c->ticksleep -= 1;
            else if (avdiff < -500) c->ticksleep += 3;
            else if (avdiff < -50 ) c->ticksleep += 2;
            else if (avdiff < -30 ) c->ticksleep += 1;
        }
        if (c->ticksleep < 0) c->ticksleep = 0;
        //-- frame rate & av sync control --//
    } else {
        c->ticksleep = c->tickframe;
    }

    if (c->ticksleep > 0 && c->cmnvars->init_params->avts_syncmode != AVSYNC_MODE_LIVE_SYNC0) av_usleep(c->ticksleep * 1000);
    av_log(NULL, AV_LOG_INFO, "d: %3d, s: %3d\n", avdiff, c->ticksleep);
}

#ifdef WIN32
void vdev_win32_render_overlay(void *ctxt, HDC hdc, int erase)
{
    VDEV_COMMON_CTXT *c   = (VDEV_COMMON_CTXT*)ctxt;
    BLENDFUNCTION     func= {0};
    RECT              rect= {0};
    int               i;

    if (c->overlay_rects[0].type == 0) { c->status |= VDEV_CLEAR; return; }
    for (i=0; i<sizeof(c->overlay_rects)/sizeof(c->overlay_rects[0]) && c->overlay_rects[i].type != 0; i++) {
        if (erase) {
            int oleft  = c->overlay_rects[i].dstx;
            int oright = c->overlay_rects[i].dstx + c->overlay_rects[i].dstw;
            int otop   = c->overlay_rects[i].dsty;
            int obottom= c->overlay_rects[i].dsty + c->overlay_rects[i].dsth;
            if (c->rectv.top > otop) {
                rect.left   = oleft;
                rect.right  = oright;
                rect.top    = otop;
                rect.bottom = MIN(obottom, c->rectv.top);
                FillRect(hdc, &rect, GetStockObject(BLACK_BRUSH));
            }
            if (c->rectv.bottom < obottom) {
                rect.left   = oleft;
                rect.right  = oright;
                rect.top    = MAX(otop, c->rectv.bottom);
                rect.bottom = obottom;
                FillRect(hdc, &rect, GetStockObject(BLACK_BRUSH));
            }
            if (c->rectv.left > oleft) {
                rect.left   = oleft;
                rect.right  = MIN(oright , c->rectv.left  );
                rect.top    = MAX(otop   , c->rectv.top   );
                rect.bottom = MIN(obottom, c->rectv.bottom);
                FillRect(hdc, &rect, GetStockObject(BLACK_BRUSH));
            }
            if (c->rectv.right < oright) {
                rect.left   = MAX(oleft  , c->rectv.right );
                rect.right  = oright;
                rect.top    = MAX(otop   , c->rectv.top   );
                rect.bottom = MIN(obottom, c->rectv.bottom);
                FillRect(hdc, &rect, GetStockObject(BLACK_BRUSH));
            }
        }
        func.BlendOp             = AC_SRC_OVER;
        func.SourceConstantAlpha = c->overlay_rects[i].type == OVERLAY_COPY_BITBLT ? 255 : c->overlay_rects[i].alpha;
        func.AlphaFormat         = c->overlay_rects[i].type == OVERLAY_PIXEL_ALPHA ? AC_SRC_ALPHA : 0;
        if (c->overlay_rects[i].type == OVERLAY_TRANSPARENT) {
            TransparentBlt(hdc, c->overlay_rects[i].dstx, c->overlay_rects[i].dsty, c->overlay_rects[i].dstw, c->overlay_rects[i].dsth,
                   c->hoverlay, c->overlay_rects[i].srcx, c->overlay_rects[i].srcy, c->overlay_rects[i].srcw, c->overlay_rects[i].srch, c->overlay_rects[i].transparent);
        } else {
            AlphaBlend(hdc, c->overlay_rects[i].dstx, c->overlay_rects[i].dsty, c->overlay_rects[i].dstw, c->overlay_rects[i].dsth,
               c->hoverlay, c->overlay_rects[i].srcx, c->overlay_rects[i].srcy, c->overlay_rects[i].srcw, c->overlay_rects[i].srch, func);
        }
    }
}
#endif
