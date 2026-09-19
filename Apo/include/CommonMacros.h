#pragma once
#include <windef.h>
#include <windows.h>

#define IF_TRUE_JUMP(condition, label)                          \
    if (condition)                                              \
    {                                                           \
        goto label;                                             \
    }

#define IF_FALSE_JUMP(condition, label)                         \
    if (!condition)                                             \
    {                                                           \
        goto label;                                             \
    }

#define IF_FAILED_JUMP(_hresult, label)                         \
    if (FAILED(_hresult))                                       \
    {                                                           \
        goto label;                                             \
    }

#define IF_SUCCEEDED_JUMP(_hresult, label)                      \
    if (SUCCEEDED(_hresult))                                    \
    {                                                           \
        goto label;                                             \
    }

#define IF_TRUE_ACTION_JUMP(condition, action, label)           \
    if (condition)                                              \
    {                                                           \
        action;                                                 \
        goto label;                                             \
    }

#define IF_FAILED_ACTION_JUMP(_hresult, action, label)          \
    if (FAILED(_hresult))                                       \
    {                                                           \
        action;                                                 \
        goto label;                                             \
    }

#define SAFE_CLOSE_HANDLE(h)                                    \
    if (NULL != h)                                              \
    {                                                           \
        CloseHandle(h);                                         \
        h = NULL;                                               \
    }

#define SAFE_ADDREF(p)                                          \
    if (NULL != p)                                              \
    {                                                           \
        (p)->AddRef();;                                         \
    }

#define SAFE_RELEASE(p)                                         \
    if (NULL != p)                                              \
    {                                                           \
        (p)->Release();                                         \
        (p) = NULL;                                             \
    }

#define SAFE_DELETE(p)                                          \
    delete p;                                                   \
    p = NULL;

#define SAFE_DELETE_ARRAY(p)                                    \
    delete [] p;                                                \
    p = NULL;

#define SAFE_COTASKMEMFREE(p)                                   \
    if (NULL != p)                                              \
    {                                                           \
        CoTaskMemFree(p);                                       \
        (p) = NULL;                                             \
    }

#define SAFE_FREELIBRARY(h)                                     \
    if (NULL != h)                                              \
    {                                                           \
        FreeLibrary(h);                                         \
        (h) = NULL;                                             \
    }

#define IS_VALID_READ_POINTER(p, s)     ((NULL != p) || (0 == s))

#define IS_VALID_WRITE_POINTER(p, s)    ((NULL != p) || (0 == s))

#define IS_VALID_TYPED_READ_POINTER(p)  IS_VALID_READ_POINTER((p), sizeof *(p))

#define IS_VALID_TYPED_WRITE_POINTER(p) IS_VALID_WRITE_POINTER((p), sizeof *(p))

#if !defined Static_SetIcon
#define Static_SetIcon(hwnd, hi) \
            (BOOL)SNDMSG((hwnd), STM_SETIMAGE, (WPARAM)IMAGE_ICON, (LPARAM)(hi))
#endif

#define TrackBar_SetTickFrequency(hwnd, f) \
            (BOOL)SNDMSG((hwnd), TBM_SETTICFREQ, (WPARAM)(f), 0)

#define TrackBar_SetBuddy(hwnd, f, hbud) \
            (HWND)SNDMSG((hwnd), TBM_SETBUDDY, (WPARAM)(f), (LPARAM)hbud)

#define TrackBar_GetPos(hwnd) \
            (int)SNDMSG((hwnd), TBM_GETPOS, 0, 0)

#define TrackBar_SetPos(hwnd, pos) \
            SNDMSG((hwnd), TBM_SETPOS, (WPARAM)TRUE, (LPARAM)pos)

#define TrackBar_SetRange(hwnd, min, max) \
            SNDMSG((hwnd), TBM_SETRANGE , (WPARAM)TRUE, (LPARAM) MAKELONG(min, max))

#define TrackBar_SetThumbLength(hwnd, l) \
            SNDMSG((hwnd), TBM_SETTHUMBLENGTH, (WPARAM)l, 0);

#define TrackBar_SetPageSize(hwnd, n) \
            SNDMSG((hwnd), TBM_SETPAGESIZE, 0, (LPARAM)n)

#define Window_GetFont(hwnd) \
            (HFONT)SNDMSG((hwnd), WM_GETFONT, 0, 0)

#define Window_SetFont(hwnd, font) \
            SNDMSG((hwnd), WM_SETFONT, (WPARAM)font, FALSE)

struct SRECT    
{
    int x, y, w, h;
    SRECT()
    {
        x = y = w = h = 0;
    }
    SRECT(int X, int Y, int W, int H)
    {
        x = X; y = Y; w = W; h = H;
    }
    SRECT(RECT* prc)
    {
        x = prc->left;
        y = prc->top;
        w = prc->right - prc->left;
        h = prc->bottom - prc->top;
    }
};

#define HNS_PER_SECOND (10ull * 1000ull * 1000ull)
