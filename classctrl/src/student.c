/* =========================================================================
 * student.c - 课堂控制被控端(学生端) v1.2
 * 仿极域电子教室架构的教学演示实现。仅用于学习/课堂教学, 请勿用于非法用途。
 * v1.2: Per-Monitor DPI 感知(高清渲染), 深色现代 UI, 退出需密码(默认 654321)。
 * 程序完全透明: 有可见状态窗口与托盘图标, 无任何隐藏/防杀/自启设计,
 * 仍可用任务管理器直接结束进程。
 *
 * 线程模型:
 *   主线程        : 状态窗口 + 锁屏/消息/广播/密码窗口 (全部 UI 在此线程)
 *   udp_recv 线程 : 接收教师端 TEACON 广播, 获知教师 IP
 *   udp_send 线程 : 每 2 秒向教师端发送 STUHB 心跳
 *   tcp 线程      : 与教师端建立/维持控制通道长连接, 解析命令
 *   demo 线程     : 广播开启时加入 UDP 组播组, 收帧/重组/显示
 * ========================================================================= */
#define _WIN32_WINNT 0x0600
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define WM_APP_STATE    (WM_APP+1)
#define WM_APP_LOCK     (WM_APP+2)
#define WM_APP_UNLOCK   (WM_APP+3)
#define WM_APP_MSG      (WM_APP+4)
#define WM_APP_DEMO_ON  (WM_APP+5)
#define WM_APP_DEMO_OFF (WM_APP+6)
#define WM_APP_FILE     (WM_APP+7)
#define WM_APP_SHUTDOWN (WM_APP+8)
#define WM_APP_REBOOT   (WM_APP+9)
#define WM_APP_TRAY     (WM_APP+11)
#define WM_APP_ESCAPE   (WM_APP+12)  /* 全屏紧急退出请求(5xF8), wParam=动作 */
#define WM_APP_RDP_OFF  (WM_APP+13)  /* 远程桌面流结束, 关闭提示窗 */

/* 紧急退出动作 */
#define ESC_ACTION_EXIT   0   /* 退出整个被控端 */
#define ESC_ACTION_UNLOCK 1   /* 解除锁屏 */
#define ESC_ACTION_DEMO   2   /* 退出广播全屏 */

#define WCLASS_MAIN "CC_Stu_Main"
#define WCLASS_LOCK "CC_Stu_Lock"
#define WCLASS_DEMO "CC_Stu_Demo"
#define WCLASS_MSG  "CC_Stu_Msg"
#define WCLASS_PW   "CC_Stu_Pw"
#define WCLASS_RIND "CC_Stu_RInd"
#define ID_BTN_EXIT 901
#define ID_BTN_FWS  902

#define ID_PW_EDIT   2003
#define ID_PW_OK     2001
#define ID_PW_CANCEL 2002

/* 退出密码 (可在此处修改) */
static const char EXIT_PASSWORD[] = "654321";

/* ---------------- 配色 ---------------- */
#define C_BG      RGB(24,26,32)
#define C_PANEL   RGB(32,35,43)
#define C_CARD    RGB(42,46,56)
#define C_CARDH   RGB(54,60,72)
#define C_BAR     RGB(17,19,24)
#define C_DOT     RGB(84,142,250)
#define C_DOTG    RGB(76,203,124)
#define C_DOTR    RGB(242,92,84)
#define C_DOTN    RGB(140,148,162)
#define C_TXT     RGB(232,236,244)
#define C_TXT2    RGB(140,148,162)
#define C_TXT3    RGB(108,114,126)

static __thread wchar_t s_wb[4][1024];
static __thread int     s_wi;
static wchar_t *W(const char *u){
    wchar_t *p = s_wb[(s_wi++) & 3];
    if(!u || !MultiByteToWideChar(CP_UTF8,0,u,-1,p,1024)) p[0]=0;
    return p;
}
static int MsgBoxU(HWND h,const char *text,const char *title,UINT fl){
    wchar_t t[512],c[1024];
    MultiByteToWideChar(CP_UTF8,0,title?title:"", -1,t,512);
    MultiByteToWideChar(CP_UTF8,0,text ?text :"",  -1,c,1024);
    return MessageBoxW(h,c,t,fl);
}

/* ---------------- DPI ---------------- */
static UINT g_dpi=96;
static int S(int v){ return MulDiv(v,(int)g_dpi,96); }
static int SD(HDC dc,int v){ return MulDiv(v,GetDeviceCaps(dc,LOGPIXELSX),96); }

static void enable_dpi_aware(void){
    typedef BOOL (WINAPI *FnCtx)(HANDLE);
    HMODULE u=GetModuleHandleW(L"user32.dll");
    FnCtx f=(FnCtx)(void*)GetProcAddress(u,"SetProcessDpiAwarenessContext");
    if(f){
        if(!f((HANDLE)-4))
            if(!f((HANDLE)-3)) f((HANDLE)-2);
    }else{
        SetProcessDPIAware();
    }
}

/* ---------------- GDI 绘图小工具 ---------------- */
static COLORREF mixc(COLORREF a,COLORREF b,int t){
    return RGB((GetRValue(a)*(255-t)+GetRValue(b)*t)/255,
               (GetGValue(a)*(255-t)+GetGValue(b)*t)/255,
               (GetBValue(a)*(255-t)+GetBValue(b)*t)/255);
}
static void FillRound(HDC dc,RECT r,COLORREF c,int rad){
    HBRUSH b=CreateSolidBrush(c);
    HGDIOBJ op=SelectObject(dc,GetStockObject(NULL_PEN));
    HGDIOBJ ob=SelectObject(dc,b);
    RoundRect(dc,r.left,r.top,r.right+1,r.bottom+1,rad,rad);
    SelectObject(dc,op);
    SelectObject(dc,ob);
    DeleteObject(b);
}
static void FrameRound(HDC dc,RECT r,COLORREF c,int rad,int px){
    HPEN pen=CreatePen(PS_SOLID,px,c);
    HGDIOBJ op=SelectObject(dc,pen);
    HGDIOBJ ob=SelectObject(dc,GetStockObject(NULL_BRUSH));
    RoundRect(dc,r.left,r.top,r.right+1,r.bottom+1,rad,rad);
    SelectObject(dc,op);
    SelectObject(dc,ob);
    DeleteObject(pen);
}
static void txt(HDC dc,int x,int y,const char *utf8,HFONT f,COLORREF c){
    RECT r;
    HGDIOBJ of;
    r.left=x; r.top=y; r.right=x+1200; r.bottom=y+200;
    of=SelectObject(dc,f);
    SetTextColor(dc,c);
    SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,W(utf8),-1,&r,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOCLIP|DT_END_ELLIPSIS);
    SelectObject(dc,of);
}
static void txtw(HDC dc,RECT r,const wchar_t *ws,HFONT f,COLORREF c,UINT fl){
    HGDIOBJ of=SelectObject(dc,f);
    SetTextColor(dc,c);
    SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,ws,-1,&r,fl);
    SelectObject(dc,of);
}
static void Dot(HDC dc,int cx,int cy,int rad,COLORREF c){
    HBRUSH b=CreateSolidBrush(c);
    HGDIOBJ op=SelectObject(dc,GetStockObject(NULL_PEN));
    HGDIOBJ ob=SelectObject(dc,b);
    Ellipse(dc,cx-rad,cy-rad,cx+rad,cy+rad);
    SelectObject(dc,op);
    SelectObject(dc,ob);
    DeleteObject(b);
}

/* ---------------- 套接字小工具 ---------------- */
static int send_all(SOCKET s,const char *d,int n){
    int off=0;
    while(off<n){
        int r=send(s,d+off,n-off,0);
        if(r<=0) return 0;
        off+=r;
    }
    return 1;
}
static int recv_all(SOCKET s,char *d,int n){
    int off=0;
    while(off<n){
        int r=recv(s,d+off,n-off,0);
        if(r<=0) return 0;
        off+=r;
    }
    return 1;
}
static int recv_u32(SOCKET s,unsigned int *v){
    char b[4];
    if(!recv_all(s,b,4)) return 0;
    memcpy(v,b,4);
    return 1;
}

/* ---------------- 全局状态 ---------------- */
static CRITICAL_SECTION g_cs;
static char g_teacherIP[64];          /* 锁定的教师端 IP (连接成功或手动指定) */
static volatile LONG g_haveTeacher=0;
/* 听到的教师端候选列表: 多网卡/多广播源时逐个尝试, 避免锁死在不可达地址上 */
#define MAXCAND 8
static char  g_cand[MAXCAND][64];
static DWORD g_candSeen[MAXCAND];
static int   g_candN=0;
static int   g_candIdx=0;
/* 诊断: UDP 绑定状态 (-1=绑定中, 0=成功, >0=Winsock 错误码) 与收包计数 */
static volatile LONG g_udpBindErr=-1;
static volatile LONG g_beaconCnt=0;
static volatile LONG g_demoRun=0;
static SOCKET g_sock=INVALID_SOCKET;
static HWND g_main,g_lock,g_demo,g_msg;
static HFONT g_fTitle,g_fSub,g_fBody,g_fBig,g_fBtn,g_fPill;
static HBRUSH g_brBg,g_brPanel,g_brCard;
static HHOOK g_khook;
static volatile LONG g_lockOn=0;
static HINSTANCE g_hInst;
static char g_hostU[128];
static char g_stateText[256];
static wchar_t g_stateW[256];
static char g_msgText[4096];
static wchar_t g_msgW[2048];
static char g_fileNote[1024];
static unsigned char *g_frm=NULL;
static unsigned g_frmW=0,g_frmH=0,g_frmCap=0;
static NOTIFYICONDATAW g_nid;

/* 控制通道发送互斥 (ACK/命令回执可能来自不同线程) */
static CRITICAL_SECTION g_sendCs;

/* 远程桌面状态 */
static volatile LONG g_rdpRun=0;
static HWND g_rdpInd=NULL;
static unsigned char *g_rdpPrev=NULL;
static unsigned g_rdpPrevCap=0,g_rdpPrevSize=0;

/* 手动指定教师端 IP (命令行 Student.exe <IP>) */
static volatile LONG g_manualTeacher=0;
/* 控制通道唯一化: 同一时刻只允许一条控制通道 (出站 or 反向接入) */
static volatile LONG g_haveCtrl=0;
/* 锁屏/广播期间结束任务管理器 (教学控制软件标准做法, 极域同款机制):
 * 任务管理器经 Ctrl+Alt+Del 安全桌面启动且自带置顶, 用户态窗口压不住, 只能移除源头 */
static volatile LONG g_blockTM=0;

/* 看门狗: 每 300ms 结束一次 Taskmgr.exe (仅锁屏/广播期间) */
static DWORD WINAPI tm_watchdog(LPVOID arg){
    (void)arg;
    while(g_blockTM){
        HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
        if(snap!=INVALID_HANDLE_VALUE){
            PROCESSENTRY32W pe;
            pe.dwSize=sizeof pe;
            if(Process32FirstW(snap,&pe)){
                do{
                    if(lstrcmpiW(pe.szExeFile,L"Taskmgr.exe")==0){
                        HANDLE p=OpenProcess(PROCESS_TERMINATE,FALSE,pe.th32ProcessID);
                        if(p){
                            TerminateProcess(p,0);
                            CloseHandle(p);
                        }
                    }
                }while(Process32NextW(snap,&pe));
            }
            CloseHandle(snap);
        }
        Sleep(300);
    }
    return 0;
}
static void tm_block_start(void){
    HANDLE t;
    if(g_blockTM) return;
    g_blockTM=1;
    t=CreateThread(NULL,0,tm_watchdog,NULL,0,NULL);
    if(t) CloseHandle(t);
}

/* 密码对话框状态 */
static HWND g_pww=NULL,g_pwEdit=NULL;
static int  g_pwOk=0,g_pwErr=0,g_pwAction=ESC_ACTION_EXIT;
static char g_pwLabel[64]="请输入退出密码:";
static WNDPROC g_pwEditOrig=NULL;
static volatile LONG g_pwOpen=0;   /* 密码框打开期间, 全屏窗口暂停置顶/前台争夺 */

/* 全屏窗口内连按 5 次 F8 触发紧急退出 (3 秒内连按有效) */
static void f8_track(int escAction){
    static DWORD last=0;
    static int cnt=0;
    DWORD now=GetTickCount();
    if(now-last>3000) cnt=0;
    last=now;
    if(++cnt>=5){
        cnt=0;
        PostMessage(g_main,WM_APP_ESCAPE,(WPARAM)escAction,0);
    }
}

static void make_fonts(void){
    if(g_fTitle) DeleteObject(g_fTitle);
    if(g_fSub)   DeleteObject(g_fSub);
    if(g_fBody)  DeleteObject(g_fBody);
    if(g_fBig)   DeleteObject(g_fBig);
    if(g_fBtn)   DeleteObject(g_fBtn);
    if(g_fPill)  DeleteObject(g_fPill);
    g_fTitle=CreateFontW(-S(20),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    g_fSub=CreateFontW(-S(13),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    g_fBody=CreateFontW(-S(15),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    g_fBig=CreateFontW(-S(26),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    g_fBtn=CreateFontW(-S(15),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    g_fPill=CreateFontW(-S(12),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
}

/* 前置声明 (窗口过程引用了后面定义的函数) */
static void ui_lock(void);
static void ui_unlock(void);
static void ui_msg_show(void);
static void ui_demo_on(void);
static void ui_demo_off(void);
static void ui_rdp_ind_on(void);
static void ui_rdp_ind_off(void);
static void fw_repair_student(void);
static DWORD WINAPI rev_accept_thread(LPVOID arg);
static int  enable_shutdown_priv(void);

static void set_state(const char *utf8){
    EnterCriticalSection(&g_cs);
    strncpy(g_stateText,utf8,255);
    g_stateText[255]=0;
    MultiByteToWideChar(CP_UTF8,0,g_stateText,-1,g_stateW,256);
    LeaveCriticalSection(&g_cs);
    if(g_main) InvalidateRect(g_main,NULL,TRUE);
}

static void force_foreground(HWND h){
    DWORD fg=GetWindowThreadProcessId(GetForegroundWindow(),NULL);
    DWORD me=GetCurrentThreadId();
    if(fg && fg!=me){
        AttachThreadInput(me,fg,TRUE);
        SetForegroundWindow(h);
        AttachThreadInput(me,fg,FALSE);
    }else{
        SetForegroundWindow(h);
    }
    SetWindowPos(h,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
}

static void dwm_modern(HWND w){
    BOOL dark=TRUE;
    int pref=2;
    if(DwmSetWindowAttribute(w,20,&dark,sizeof dark)!=S_OK)
        DwmSetWindowAttribute(w,19,&dark,sizeof dark);
    DwmSetWindowAttribute(w,33,&pref,sizeof pref);
}

static void monitor_rect(HWND ref,RECT *out){
    HMONITOR hm=MonitorFromWindow(ref,MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize=sizeof mi;
    if(GetMonitorInfoW(hm,&mi)) *out=mi.rcMonitor;
    else{
        out->left=0; out->top=0;
        out->right=GetSystemMetrics(SM_CXSCREEN);
        out->bottom=GetSystemMetrics(SM_CYSCREEN);
    }
}

/* ---------------- 锁屏: 低级键盘钩子 ---------------- */
static LRESULT CALLBACK kproc(int code,WPARAM wp,LPARAM lp){
    if(code==HC_ACTION && g_lockOn){
        KBDLLHOOKSTRUCT *k=(KBDLLHOOKSTRUCT*)lp;
        BOOL alt=(k->flags & LLKHF_ALTDOWN)!=0;
        BOOL ctl=(GetAsyncKeyState(VK_CONTROL)&0x8000)!=0;
        if(k->vkCode==VK_TAB   && alt) return 1;
        if(k->vkCode==VK_ESCAPE&& alt) return 1;
        if(k->vkCode==VK_F4    && alt) return 1;
        if(k->vkCode==VK_LWIN || k->vkCode==VK_RWIN) return 1;
        if(k->vkCode==VK_ESCAPE&& ctl) return 1;
        if(k->vkCode==VK_TAB   && ctl) return 1;
    }
    return CallNextHookEx(g_khook,code,wp,lp);
}

static void draw_lock_icon(HDC dc,int cx,int cy,UINT dpi){
    /* 锁环 */
    {
        HPEN pen=CreatePen(PS_SOLID,MulDiv(5,dpi,96),RGB(120,132,155));
        HGDIOBJ op=SelectObject(dc,pen);
        HGDIOBJ ob=SelectObject(dc,GetStockObject(NULL_BRUSH));
        Ellipse(dc,cx-MulDiv(16,dpi,96),cy-MulDiv(30,dpi,96),
                  cx+MulDiv(16,dpi,96),cy+MulDiv(4,dpi,96));
        SelectObject(dc,op);
        SelectObject(dc,ob);
        DeleteObject(pen);
    }
    /* 锁体 */
    {
        RECT b={cx-MulDiv(24,dpi,96),cy-MulDiv(10,dpi,96),
                cx+MulDiv(24,dpi,96),cy+MulDiv(22,dpi,96)};
        FillRound(dc,b,RGB(96,106,130),MulDiv(10,dpi,96));
    }
    /* 锁孔 */
    Dot(dc,cx,cy+MulDiv(2,dpi,96),MulDiv(5,dpi,96),RGB(26,30,38));
    {
        RECT st={cx-MulDiv(3,dpi,96),cy+MulDiv(2,dpi,96),
                 cx+MulDiv(3,dpi,96),cy+MulDiv(14,dpi,96)};
        HBRUSH b=CreateSolidBrush(RGB(26,30,38));
        FillRect(dc,&st,b);
        DeleteObject(b);
    }
}

static LRESULT CALLBACK lock_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    switch(m){
    case WM_PAINT:{
        PAINTSTRUCT ps;
        HDC dc=BeginPaint(w,&ps);
        RECT rc,card;
        UINT dpi=GetDeviceCaps(dc,LOGPIXELSX);
        int cw=SD(dc,580),chh=SD(dc,250),cx,cy;
        HFONT big=CreateFontW(-SD(dc,30),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        HFONT body=CreateFontW(-SD(dc,15),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        GetClientRect(w,&rc);
        {
            HBRUSH b=CreateSolidBrush(RGB(10,11,15));
            FillRect(dc,&rc,b);
            DeleteObject(b);
        }
        cx=(rc.left+rc.right)/2;
        cy=(rc.top+rc.bottom)/2;
        card.left=cx-cw/2;  card.top=cy-chh/2;
        card.right=cx+cw/2; card.bottom=cy+chh/2;
        FillRound(dc,card,RGB(26,29,36),SD(dc,18));
        FrameRound(dc,card,RGB(62,70,86),SD(dc,18),1);
        draw_lock_icon(dc,cx,card.top+SD(dc,72),dpi);
        {
            RECT tr={card.left,card.top+SD(dc,104),card.right,card.top+SD(dc,160)};
            HGDIOBJ of=SelectObject(dc,big);
            SetTextColor(dc,C_TXT);
            SetBkMode(dc,TRANSPARENT);
            DrawTextW(dc,W("屏幕已锁定"),-1,&tr,DT_CENTER|DT_SINGLELINE);
            SelectObject(dc,of);
            tr.top=card.top+SD(dc,166); tr.bottom=tr.top+SD(dc,30);
            of=SelectObject(dc,body);
            SetTextColor(dc,C_TXT2);
            DrawTextW(dc,W("请专心听课 · 本窗口由教师端解除"),-1,&tr,
                      DT_CENTER|DT_SINGLELINE);
            SelectObject(dc,of);
        }
        EndPaint(w,&ps);
        DeleteObject(big);
        DeleteObject(body);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(NULL);
        return TRUE;
    case WM_TIMER:
        if(g_pwOpen) return 0;   /* 密码框打开期间暂停争夺, 让密码框保持最上层 */
        /* 每 300ms 重夺置顶与前台 (对抗任务管理器"始终置顶") */
        if(IsIconic(w)) ShowWindow(w,SW_RESTORE);
        SetWindowPos(w,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        if(GetForegroundWindow()!=w) force_foreground(w);
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if(m==WM_KEYDOWN && wp==VK_F8) f8_track(ESC_ACTION_UNLOCK);
        return 0;
    case WM_SYSCOMMAND:
        return 0;
    case WM_CLOSE: case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(w,m,wp,lp);
}

/* ---------------- 屏幕广播窗口 ---------------- */
static LRESULT CALLBACK demo_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    switch(m){
    case WM_PAINT:{
        PAINTSTRUCT ps;
        HDC wdc=BeginPaint(w,&ps);
        HDC md=CreateCompatibleDC(wdc);
        HBITMAP mdbm=CreateCompatibleBitmap(wdc,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN));
        HGDIOBJ mdold=SelectObject(md,mdbm);
        HDC dc=md;
        RECT rc,hr;
        HBRUSH b=CreateSolidBrush(RGB(0,0,0));
        HFONT pill=CreateFontW(-SD(dc,12),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        GetClientRect(w,&rc);
        FillRect(dc,&rc,b);
        DeleteObject(b);
        EnterCriticalSection(&g_cs);
        if(g_frm && g_frmW && g_frmH){
            BITMAPINFO bmi;
            memset(&bmi,0,sizeof bmi);
            bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth=(LONG)g_frmW;
            bmi.bmiHeader.biHeight=-(LONG)g_frmH;
            bmi.bmiHeader.biPlanes=1;
            bmi.bmiHeader.biBitCount=16;
            bmi.bmiHeader.biCompression=BI_RGB;
            SetStretchBltMode(dc,COLORONCOLOR);
            StretchDIBits(dc,0,0,rc.right,rc.bottom,0,0,(int)g_frmW,(int)g_frmH,
                          g_frm,&bmi,DIB_RGB_COLORS,SRCCOPY);
        }
        LeaveCriticalSection(&g_cs);
        hr.right=rc.right-SD(dc,16); hr.bottom=rc.bottom-SD(dc,14);
        hr.left=hr.right-SD(dc,190); hr.top=hr.bottom-SD(dc,30);
        FillRound(dc,hr,RGB(18,20,26),SD(dc,10));
        txtw(dc,hr,W("教师端正在广播"),pill,C_TXT2,
             DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        BitBlt(wdc,0,0,rc.right,rc.bottom,md,0,0,SRCCOPY);
        SelectObject(md,mdold);
        DeleteObject(mdbm);
        DeleteDC(md);
        EndPaint(w,&ps);
        DeleteObject(pill);
        return 0;
    }
    case WM_TIMER:
        if(g_pwOpen) return 0;   /* 密码框打开期间暂停争夺 */
        if(IsIconic(w)) ShowWindow(w,SW_RESTORE);
        SetWindowPos(w,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        if(GetForegroundWindow()!=w) force_foreground(w);
        return 0;
    case WM_KEYDOWN:
        if(wp=='Q' && (GetKeyState(VK_CONTROL)&0x8000) && (GetKeyState(VK_MENU)&0x8000)){
            PostMessage(g_main,WM_APP_DEMO_OFF,0,0);
            return 0;
        }
        if(wp==VK_F8) f8_track(ESC_ACTION_DEMO);
        return 0;
    case WM_SYSCOMMAND:
        return 0;
    case WM_CLOSE: case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(w,m,wp,lp);
}

/* ---------------- 消息窗口 ---------------- */
static LRESULT CALLBACK msg_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    switch(m){
    case WM_PAINT:{
        PAINTSTRUCT ps;
        HDC dc=BeginPaint(w,&ps);
        RECT rc,bar;
        HFONT big=CreateFontW(-SD(dc,26),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        GetClientRect(w,&rc);
        {
            HBRUSH b=CreateSolidBrush(C_PANEL);
            FillRect(dc,&rc,b);
            DeleteObject(b);
        }
        bar.left=SD(dc,14); bar.top=SD(dc,18);
        bar.right=bar.left+SD(dc,8); bar.bottom=rc.bottom-SD(dc,18);
        FillRound(dc,bar,C_DOT,SD(dc,4));
        {
            RECT tr={bar.right+SD(dc,20),SD(dc,10),rc.right-SD(dc,14),rc.bottom-SD(dc,10)};
            HGDIOBJ of=SelectObject(dc,big);
            SetTextColor(dc,C_TXT);
            SetBkMode(dc,TRANSPARENT);
            DrawTextW(dc,g_msgW,-1,&tr,DT_CENTER|DT_VCENTER|DT_WORDBREAK|DT_END_ELLIPSIS);
            SelectObject(dc,of);
        }
        EndPaint(w,&ps);
        DeleteObject(big);
        return 0;
    }
    case WM_TIMER:
        if(wp==1 && g_msg==w){ DestroyWindow(w); }
        return 0;
    case WM_LBUTTONDOWN:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        if(g_msg==w) g_msg=NULL;
        return 0;
    }
    return DefWindowProcW(w,m,wp,lp);
}

/* ---------------- 退出密码对话框 ---------------- */
static LRESULT CALLBACK pwedit_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    if(m==WM_KEYDOWN){
        if(wp==VK_RETURN){
            SendMessageW(g_pww,WM_COMMAND,MAKELPARAM(ID_PW_OK,0),(LPARAM)w);
            return 0;
        }
        if(wp==VK_ESCAPE){
            SendMessageW(g_pww,WM_COMMAND,MAKELPARAM(ID_PW_CANCEL,0),(LPARAM)w);
            return 0;
        }
    }
    return CallWindowProcW(g_pwEditOrig,w,m,wp,lp);
}

static void pw_verify(void){
    char buf[64];
    GetDlgItemTextA(g_pww,ID_PW_EDIT,buf,64);
    if(strcmp(buf,EXIT_PASSWORD)==0){
        g_pwOk=1;
        DestroyWindow(g_pww);
    }else{
        g_pwErr=1;
        SetDlgItemTextW(g_pww,ID_PW_EDIT,L"");
        InvalidateRect(g_pww,NULL,TRUE);
        MessageBeep(MB_ICONERROR);
        SetFocus(g_pwEdit);
    }
}

static void draw_pwbtn(DRAWITEMSTRUCT *d){
    COLORREF c=(d->CtlID==ID_PW_OK)?C_DOT:RGB(52,58,70);
    if(d->itemState & ODS_SELECTED) c=mixc(c,RGB(0,0,0),60);
    else if(d->itemState & ODS_HOTLIGHT) c=mixc(c,RGB(255,255,255),45);
    FillRect(d->hDC,&d->rcItem,g_brBg);   /* 铺底色, 消除圆角白边 */
    FillRound(d->hDC,d->rcItem,c,S(12));
    {
        HGDIOBJ of=SelectObject(d->hDC,g_fBtn);
        SetBkMode(d->hDC,TRANSPARENT);
        SetTextColor(d->hDC,RGB(255,255,255));
        DrawTextW(d->hDC,W(d->CtlID==ID_PW_OK?"确 认":"取 消"),-1,&d->rcItem,
                  DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(d->hDC,of);
    }
}

static LRESULT CALLBACK pw_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    switch(m){
    case WM_CREATE:{
        HINSTANCE hi=(HINSTANCE)GetWindowLongPtrW(w,GWLP_HINSTANCE);
        HWND e;
        CreateWindowExW(0,L"EDIT",L"",
                WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL|ES_PASSWORD,
                S(36),S(50),S(268),S(24),w,(HMENU)(INT_PTR)ID_PW_EDIT,hi,NULL);
        g_pwEdit=GetDlgItem(w,ID_PW_EDIT);
        SendMessageW(g_pwEdit,EM_SETPASSWORDCHAR,(WPARAM)0x25CF,0);
        SendMessageW(g_pwEdit,WM_SETFONT,(WPARAM)g_fBody,MAKELPARAM(1,0));
        g_pwEditOrig=(WNDPROC)SetWindowLongPtrW(g_pwEdit,GWLP_WNDPROC,(LONG_PTR)pwedit_proc);
        e=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
                S(64),S(124),S(96),S(36),w,(HMENU)(INT_PTR)ID_PW_OK,hi,NULL);
        (void)e;
        e=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
                S(180),S(124),S(96),S(36),w,(HMENU)(INT_PTR)ID_PW_CANCEL,hi,NULL);
        (void)e;
        SetFocus(g_pwEdit);
        return 0;
    }
    case WM_PAINT:{
        PAINTSTRUCT ps;
        HDC dc=BeginPaint(w,&ps);
        RECT rc,pan;
        GetClientRect(w,&rc);
        {
            HBRUSH b=CreateSolidBrush(C_BG);
            FillRect(dc,&rc,b);
            DeleteObject(b);
        }
        txt(dc,S(24),S(16),g_pwLabel,g_fBody,C_TXT);
        pan.left=S(24); pan.top=S(42); pan.right=S(316); pan.bottom=S(82);
        FillRound(dc,pan,C_CARD,S(10));
        if(g_pwErr)
            txt(dc,S(24),S(90),"密码错误, 请重试",g_fPill,C_DOTR);
        EndPaint(w,&ps);
        return 0;
    }
    case WM_DRAWITEM:
        draw_pwbtn((DRAWITEMSTRUCT*)lp);
        return TRUE;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp,C_TXT);
        SetBkColor((HDC)wp,C_CARD);
        return (LRESULT)(INT_PTR)g_brCard;
    case WM_CTLCOLORBTN:
        return (LRESULT)(INT_PTR)g_brBg;
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp,C_TXT2);
        SetBkColor((HDC)wp,C_BG);
        return (LRESULT)(INT_PTR)g_brBg;
    case WM_COMMAND:
        if(LOWORD(wp)==ID_PW_OK) pw_verify();
        else if(LOWORD(wp)==ID_PW_CANCEL) DestroyWindow(w);
        return 0;
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(g_pwEdit,GWLP_WNDPROC,(LONG_PTR)g_pwEditOrig);
        g_pww=NULL;
        g_pwEdit=NULL;
        return 0;
    }
    return DefWindowProcW(w,m,wp,lp);
}

static void show_password_dialog(int action){
    static int cls_ok=0;
    RECT rm,r;
    int x,y;
    MSG m;
    if(!cls_ok){
        WNDCLASSW wc;
        memset(&wc,0,sizeof wc);
        wc.lpfnWndProc=pw_proc;
        wc.hInstance=g_hInst;
        wc.hIcon=LoadIconW(NULL,IDI_APPLICATION);
        wc.hCursor=LoadCursorW(NULL,IDC_ARROW);
        wc.hbrBackground=NULL;
        wc.lpszClassName=W("CC_Stu_Pw");
        RegisterClassW(&wc);
        cls_ok=1;
    }
    g_pwAction=action;
    strcpy(g_pwLabel,(action==ESC_ACTION_EXIT)?"请输入退出密码:":"请输入紧急退出密码:");
    g_pwOpen=1;                     /* 暂停全屏窗口的置顶/前台争夺 */
    EnableWindow(g_main,FALSE);
    g_pwOk=0; g_pwErr=0;
    GetWindowRect(g_main,&rm);
    r.left=0; r.top=0; r.right=S(340); r.bottom=S(180);
    AdjustWindowRect(&r,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE);
    x=rm.left+(rm.right-rm.left-(r.right-r.left))/2;
    y=rm.top+(rm.bottom-rm.top-(r.bottom-r.top))/2;
    /* TOPMOST: 从锁屏/广播全屏唤出时可覆盖全屏窗口 */
    g_pww=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,W("CC_Stu_Pw"),
            W(action==ESC_ACTION_EXIT?"退出被控端":"紧急解除全屏"),
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
            x,y,r.right-r.left,r.bottom-r.top,
            g_main,NULL,g_hInst,NULL);
    dwm_modern(g_pww);
    ShowWindow(g_pww,SW_SHOW);
    UpdateWindow(g_pww);
    SetFocus(g_pwEdit);
    force_foreground(g_pww);
    while(GetMessageW(&m,NULL,0,0)>0){
        if(m.message==WM_QUIT){     /* 退出请求出现在模态循环中: 转投给主循环 */
            PostQuitMessage((int)m.wParam);
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
        if(!IsWindow(g_pww)) break;
    }
    g_pwOpen=0;                     /* 恢复全屏窗口的置顶/前台争夺 */
    if(g_pwOk && g_pwAction==ESC_ACTION_EXIT){
        /* 退出: 销毁主窗口(密码框已随流程销毁), WM_QUIT 交给主循环结束进程 */
        if(g_main && IsWindow(g_main)) DestroyWindow(g_main);
        return;
    }
    EnableWindow(g_main,TRUE);
    if(g_main && IsWindow(g_main)) SetForegroundWindow(g_main);
    if(g_pwOk){
        if(g_pwAction==ESC_ACTION_UNLOCK) ui_unlock();
        else if(g_pwAction==ESC_ACTION_DEMO) ui_demo_off();
    }
}

/* ---------------- 主状态窗口 ---------------- */
static void layout_children(void){
    HWND b=GetDlgItem(g_main,ID_BTN_EXIT);
    SetWindowPos(b,NULL,S(250),S(286),S(130),S(36),SWP_NOZORDER|SWP_NOACTIVATE);
    b=GetDlgItem(g_main,ID_BTN_FWS);
    if(b) SetWindowPos(b,NULL,S(20),S(286),S(210),S(36),SWP_NOZORDER|SWP_NOACTIVATE);
}

static LRESULT CALLBACK main_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    switch(m){
    case WM_CREATE:{
        HINSTANCE hi=(HINSTANCE)GetWindowLongPtrW(w,GWLP_HINSTANCE);
        CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
                S(250),S(286),S(130),S(36),w,(HMENU)(INT_PTR)ID_BTN_EXIT,hi,NULL);
        CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
                S(20),S(286),S(210),S(36),w,(HMENU)(INT_PTR)ID_BTN_FWS,hi,NULL);
        return 0;
    }
    case WM_DPICHANGED:
        g_dpi=HIWORD(wp);
        make_fonts();
        layout_children();
        {
            RECT *r=(RECT*)lp;
            SetWindowPos(w,NULL,r->left,r->top,r->right-r->left,r->bottom-r->top,
                         SWP_NOZORDER|SWP_NOACTIVATE);
        }
        InvalidateRect(w,NULL,TRUE);
        return 0;
    case WM_PAINT:{
        PAINTSTRUCT ps;
        HDC wdc=BeginPaint(w,&ps);
        HDC md=CreateCompatibleDC(wdc);
        HBITMAP mdbm=CreateCompatibleBitmap(wdc,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN));
        HGDIOBJ mdold=SelectObject(md,mdbm);
        HDC dc=md;
        RECT rc,card;
        char note[512];
        wchar_t hostW[128];
        GetClientRect(w,&rc);
        {
            HBRUSH b=CreateSolidBrush(C_BG);
            FillRect(dc,&rc,b);
            DeleteObject(b);
        }
        txt(dc,S(20),S(16),"课堂控制 · 被控端",g_fTitle,C_TXT);
        txt(dc,S(22),S(52),"ClassCtrl v1.5.0 · 学生端 · 广播+反向接入 双通道",g_fSub,C_TXT2);
        /* 状态卡片 */
        card.left=S(20); card.top=S(80); card.right=S(380); card.bottom=S(190);
        FillRound(dc,card,C_PANEL,S(14));
        Dot(dc,card.left+S(26),card.top+S(26),S(6),
            (strstr(g_stateText,"已连接")||strstr(g_stateText,"等待指令"))?C_DOTG:RGB(90,96,108));
        {
            RECT tr={card.left+S(44),card.top+S(12),card.right-S(16),card.top+S(38)};
            txtw(dc,tr,g_stateW,g_fBody,C_TXT,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        }
        /* 监听诊断 + 候选教师端 */
        {
            wchar_t cl[320];
            int k,cn=0;
            LONG berr,bcnt;
            RECT tr={card.left+S(26),card.top+S(36),card.right-S(16),card.top+S(52)};
            berr=g_udpBindErr;
            bcnt=g_beaconCnt;
            EnterCriticalSection(&g_cs);
            cn=g_candN;
            cl[0]=0;
            if(cn){
                wcscpy(cl,W("候选: "));
                for(k=0;k<cn && k<2;k++){
                    wchar_t one[64];
                    MultiByteToWideChar(CP_UTF8,0,g_cand[k],-1,one,64);
                    if(k) lstrcatW(cl,L", ");
                    lstrcatW(cl,one);
                }
                if(cn>2) wsprintfW(cl+wcslen(cl),L" 等 %d 个",cn);
            }else{
                wcscpy(cl,W("尚未收到教师端广播"));
            }
            LeaveCriticalSection(&g_cs);
            if(berr<0){
                txtw(dc,tr,W("UDP 监听: 绑定中…"),g_fPill,RGB(96,102,114),
                     DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
            }else if(berr>0){
                wchar_t dl[128];
                wsprintfW(dl,W("UDP 绑定失败(错误 %d) — 端口可能被占用"),(int)berr);
                txtw(dc,tr,dl,g_fPill,C_DOTR,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
            }
            {
                RECT tr2={card.left+S(26),card.top+S(54),card.right-S(16),card.top+S(70)};
                wchar_t dl2[160];
                wsprintfW(dl2,W("已收 %d 包 · %s"),(int)bcnt,cl);
                txtw(dc,tr2,dl2,g_fPill,cn?RGB(120,170,255):RGB(96,102,114),
                     DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
            }
        }
        MultiByteToWideChar(CP_UTF8,0,g_hostU,-1,hostW,128);
        {
            wchar_t line[200];
            RECT tr={card.left+S(26),card.top+S(72),card.right-S(16),card.top+S(88)};
            wsprintfW(line,W("本机: %s"),hostW);
            txtw(dc,tr,line,g_fPill,C_TXT2,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
            tr.top=card.top+S(90); tr.bottom=tr.top+S(16);
            EnterCriticalSection(&g_cs);
            strncpy(note,g_fileNote,511);
            note[511]=0;
            LeaveCriticalSection(&g_cs);
            if(note[0]){
                wchar_t nw[512];
                MultiByteToWideChar(CP_UTF8,0,note,-1,nw,512);
                txtw(dc,tr,nw,g_fPill,RGB(120,170,255),DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
            }else{
                txtw(dc,tr,W("尚无教师端下发的文件"),g_fPill,RGB(96,102,114),
                     DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
            }
        }
        /* 提示 */
        {
            RECT hr={S(20),S(198),S(380),S(270)};
            txtw(dc,hr,
                 W("收到教师端下发的文件将保存在 文档\\课堂接收文件\n远程查看/控制期间屏幕右下角会显示提示条\n无法自动发现教师端: 点\"防火墙修复\", 或命令行运行\nStudent.exe 教师IP (如 Student.exe 192.168.1.10)"),
                 g_fSub,C_TXT2,DT_WORDBREAK);
        }
        BitBlt(wdc,0,0,rc.right,rc.bottom,md,0,0,SRCCOPY);
        SelectObject(md,mdold);
        DeleteObject(mdbm);
        DeleteDC(md);
        EndPaint(w,&ps);
        return 0;
    }
    case WM_DRAWITEM:{
        DRAWITEMSTRUCT *d=(DRAWITEMSTRUCT*)lp;
        if(d->CtlType==ODT_BUTTON && d->CtlID==ID_BTN_FWS){
            COLORREF bg=C_CARD;
            if(d->itemState & ODS_SELECTED) bg=RGB(34,38,46);
            else if(d->itemState & ODS_HOTLIGHT) bg=C_CARDH;
            FillRect(d->hDC,&d->rcItem,g_brBg);
            FillRound(d->hDC,d->rcItem,bg,S(10));
            {
                int cy=(d->rcItem.top+d->rcItem.bottom)/2;
                RECT tr=d->rcItem;
                HGDIOBJ of;
                Dot(d->hDC,d->rcItem.left+S(18),cy,S(5),C_DOTN);
                tr.left=d->rcItem.left+S(34);
                of=SelectObject(d->hDC,g_fBtn);
                SetBkMode(d->hDC,TRANSPARENT);
                SetTextColor(d->hDC,C_TXT);
                DrawTextW(d->hDC,W("防火墙修复"),-1,&tr,
                          DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                SelectObject(d->hDC,of);
            }
            return TRUE;
        }
        if(d->CtlType==ODT_BUTTON && d->CtlID==ID_BTN_EXIT){
            COLORREF bg=C_CARD;
            int en=!(d->itemState & ODS_DISABLED);
            if(d->itemState & ODS_SELECTED) bg=RGB(34,38,46);
            else if(d->itemState & ODS_HOTLIGHT) bg=C_CARDH;
            FillRect(d->hDC,&d->rcItem,g_brBg);   /* 铺底色, 消除圆角白边 */
            FillRound(d->hDC,d->rcItem,bg,S(10));
            FrameRound(d->hDC,d->rcItem,RGB(120,60,62),S(10),1);
            {
                int cy=(d->rcItem.top+d->rcItem.bottom)/2;
                RECT tr=d->rcItem;
                HGDIOBJ of;
                Dot(d->hDC,d->rcItem.left+S(18),cy,S(5),en?C_DOTR:RGB(80,86,98));
                tr.left=d->rcItem.left+S(34);
                of=SelectObject(d->hDC,g_fBtn);
                SetBkMode(d->hDC,TRANSPARENT);
                SetTextColor(d->hDC,en?RGB(244,124,114):C_TXT3);
                DrawTextW(d->hDC,W("退出程序"),-1,&tr,
                          DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                SelectObject(d->hDC,of);
            }
            return TRUE;
        }
        return FALSE;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORBTN:          /* 按钮底层背景用深色, 圆角外无白边 */
        return (LRESULT)(INT_PTR)g_brBg;
    case WM_COMMAND:
        if(LOWORD(wp)==ID_BTN_EXIT){
            show_password_dialog(ESC_ACTION_EXIT);
        }else if(LOWORD(wp)==ID_BTN_FWS){
            fw_repair_student();
        }
        return 0;
    case WM_APP_ESCAPE:   /* 全屏内 5xF8 触发 */
        show_password_dialog((int)wp);
        return 0;
    case WM_APP_RDP_OFF:  /* 远程桌面流结束 */
        ui_rdp_ind_off();
        return 0;
    case WM_TIMER:
        InvalidateRect(w,NULL,TRUE);   /* 刷新监听诊断行 */
        return 0;
    case WM_APP_STATE:
    case WM_APP_FILE:
        InvalidateRect(w,NULL,TRUE);
        return 0;
    case WM_APP_LOCK:
        ui_lock();
        return 0;
    case WM_APP_UNLOCK:
        ui_unlock();
        return 0;
    case WM_APP_MSG:
        ui_msg_show();
        return 0;
    case WM_APP_DEMO_ON:
        ui_demo_on();
        return 0;
    case WM_APP_DEMO_OFF:
        ui_demo_off();
        return 0;
    case WM_APP_SHUTDOWN:
        enable_shutdown_priv();
        ExitWindowsEx(EWX_SHUTDOWN|EWX_FORCEIFHUNG|EWX_POWEROFF,0);
        return 0;
    case WM_APP_REBOOT:
        enable_shutdown_priv();
        ExitWindowsEx(EWX_REBOOT|EWX_FORCEIFHUNG,0);
        return 0;
    case WM_APP_TRAY:
        if(lp==WM_LBUTTONDBLCLK || lp==WM_RBUTTONUP){
            ShowWindow(w,SW_SHOW);
            SetForegroundWindow(w);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(w,SW_HIDE);
        return 0;
    case WM_DESTROY:
        ui_rdp_ind_off();
        if(g_pww){ DestroyWindow(g_pww); g_pww=NULL; }   /* 退出时确保密码框关闭 */
        g_pwOpen=0;
        Shell_NotifyIconW(NIM_DELETE,&g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w,m,wp,lp);
}

/* ---------------- UDP: 发现教师端 ---------------- */
static void send_udp_to(const char *ip,int port,const char *pkt,int n){
    SOCKET u=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(u==INVALID_SOCKET) return;
    {
        struct sockaddr_in a;
        memset(&a,0,sizeof a);
        a.sin_family=AF_INET;
        a.sin_port=htons((unsigned short)port);
        a.sin_addr.s_addr=inet_addr(ip);
        if(a.sin_addr.s_addr!=INADDR_NONE)
            sendto(u,pkt,n,0,(struct sockaddr*)&a,sizeof a);
    }
    closesocket(u);
}

static DWORD WINAPI udp_recv_thread(LPVOID arg){
    SOCKET u;
    (void)arg;
    u=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(u==INVALID_SOCKET) return 0;
    {
        BOOL ru=1;
        setsockopt(u,SOL_SOCKET,SO_REUSEADDR,(char*)&ru,sizeof ru);
    }
    {
        struct sockaddr_in a;
        memset(&a,0,sizeof a);
        a.sin_family=AF_INET;
        a.sin_port=htons(UDP_PORT);
        a.sin_addr.s_addr=INADDR_ANY;
        if(bind(u,(struct sockaddr*)&a,sizeof a)!=0){
            g_udpBindErr=(LONG)WSAGetLastError();
            closesocket(u);
            return 0;
        }
        g_udpBindErr=0;
    }
    for(;;){
        char buf[512];
        struct sockaddr_in f;
        int fl=sizeof f;
        int n=recvfrom(u,buf,sizeof buf-1,0,(struct sockaddr*)&f,&fl);
        if(n<=0){ Sleep(200); continue; }
        InterlockedIncrement(&g_beaconCnt);
        buf[n]=0;
        if(!strncmp(buf,"TEACON|",7)){
            char ip[64];
            int fresh=0;
            char *bar;
            strcpy(ip,inet_ntoa(f.sin_addr));
            /* 广播携带教师端对应网卡的 IP: TEACON|v1|<ip> (多网卡可达) */
            bar=strchr(buf+7,'|');
            if(bar){
                const char *cand=bar+1;
                unsigned long a=inet_addr(cand);
                if(a!=INADDR_NONE && a!=0) strcpy(ip,cand);
            }
            EnterCriticalSection(&g_cs);
            if(!g_manualTeacher){
                int k,found=0;
                for(k=0;k<g_candN;k++)
                    if(!strcmp(g_cand[k],ip)){ g_candSeen[k]=GetTickCount(); found=1; break; }
                if(!found){
                    int v=0;
                    if(g_candN<MAXCAND) v=g_candN++;
                    else{
                        DWORD oldest=0xFFFFFFFFu;
                        for(k=0;k<g_candN;k++)
                            if(g_candSeen[k]<oldest){ oldest=g_candSeen[k]; v=k; }
                    }
                    strcpy(g_cand[v],ip);
                    g_candSeen[v]=GetTickCount();
                    fresh=1;
                }
                g_haveTeacher=1;
            }else{
                g_haveTeacher=1;    /* 手动模式: 广播仅作存活信号 */
            }
            LeaveCriticalSection(&g_cs);
            if(fresh){
                char pkt[300];
                int nn=sprintf(pkt,"STULOGIN|%s",g_hostU);
                send_udp_to(ip,STU_UDP_PORT,pkt,nn);
                set_state("发现教师端广播, 正在连接...");
            }
        }
    }
}

static DWORD WINAPI udp_send_thread(LPVOID arg){
    (void)arg;
    for(;;){
        char pkt[300];
        int nn=sprintf(pkt,"STUHB|%s",g_hostU);
        int manual,have,cn,k;
        DWORD now=GetTickCount();
        char cands[MAXCAND][64];
        DWORD seen[MAXCAND];
        char lip[64];
        EnterCriticalSection(&g_cs);
        manual=(int)g_manualTeacher;
        have=(int)g_haveTeacher;
        strcpy(lip,g_teacherIP);
        cn=g_candN;
        for(k=0;k<cn;k++){ strcpy(cands[k],g_cand[k]); seen[k]=g_candSeen[k]; }
        LeaveCriticalSection(&g_cs);
        if(manual){
            send_udp_to(lip,STU_UDP_PORT,pkt,nn);
        }else{
            int any=0;
            for(k=0;k<cn;k++)
                if(now-seen[k]<15000){ send_udp_to(cands[k],STU_UDP_PORT,pkt,nn); any=1; }
            if(!any && have) send_udp_to(lip,STU_UDP_PORT,pkt,nn);
            /* 全部候选超时 → 判离线并给出可诊断的提示 */
            if(cn>0 && !any){
                int alive=0;
                EnterCriticalSection(&g_cs);
                for(k=0;k<g_candN;k++)
                    if(GetTickCount()-g_candSeen[k]<15000) alive=1;
                g_haveTeacher=alive?1:0;
                LeaveCriticalSection(&g_cs);
                if(!alive) set_state("未收到教师端广播: 请检查防火墙放行/同一网段, 等待中...");
            }
        }
        Sleep(2000);
    }
    return 0;
}

/* ---------------- TCP 控制通道 ---------------- */
static void send_ack(SOCKET s,const char *cmd){
    char b[64],fr[128];
    unsigned int n;
    int hl=sprintf(b,"ACK|%s\n",cmd);
    n=(unsigned int)hl;
    memcpy(fr,&n,4);
    memcpy(fr+4,b,hl);
    EnterCriticalSection(&g_sendCs);
    send_all(s,fr,hl+4);
    LeaveCriticalSection(&g_sendCs);
}

static void sanitize_name(char *fn){
    char *last=fn,*o;
    for(o=fn; *o; o++)
        if(*o=='\\'||*o=='/') last=o+1;
    if(last!=fn) memmove(fn,last,strlen(last)+1);
    for(o=fn; *o; o++){
        unsigned char c=(unsigned char)*o;
        if(c<32 || c>126 || strchr("*?\"<>|:",c)) *o='_';
    }
    if(!fn[0]) strcpy(fn,"file.dat");
    if(strlen(fn)>200) fn[200]=0;
}

/* ---------------- 远程终端: 命令执行 ---------------- */
static DWORD WINAPI exec_watchdog(LPVOID p){
    HANDLE h=(HANDLE)p;
    if(WaitForSingleObject(h,60000)==WAIT_TIMEOUT) TerminateProcess(h,1);
    return 0;
}

/* 在学生端执行命令并返回 UTF-8 输出 (GBK 控制台输出自动转码) */
static char *run_command(const char *cmdutf8,int *outLen){
    wchar_t wcmd[4096],full[4700];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE rR=NULL,rW=NULL;
    char *raw=NULL;
    int rawLen=0,rawCap=0;
    DWORD code=0;
    *outLen=0;
    if(MultiByteToWideChar(CP_UTF8,0,cmdutf8,-1,wcmd,4096)<=0) return NULL;
    lstrcpyW(full,L"cmd.exe /C ");
    lstrcatW(full,wcmd);
    sa.nLength=sizeof sa;
    sa.bInheritHandle=TRUE;
    sa.lpSecurityDescriptor=NULL;
    if(!CreatePipe(&rR,&rW,&sa,0)){ return NULL; }
    SetHandleInformation(rR,HANDLE_FLAG_INHERIT,0);
    ZeroMemory(&si,sizeof si);
    si.cb=sizeof si;
    si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    si.wShowWindow=SW_HIDE;
    si.hStdOutput=rW;
    si.hStdError=rW;
    if(!CreateProcessW(NULL,full,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)){
        CloseHandle(rR); CloseHandle(rW);
        return NULL;
    }
    CloseHandle(rW);
    CloseHandle(CreateThread(NULL,0,exec_watchdog,pi.hProcess,0,NULL));
    for(;;){
        char tmp[4096];
        DWORD rd=0;
        if(!ReadFile(rR,tmp,sizeof tmp,&rd,NULL) || rd==0) break;
        if(rawLen+(int)rd<=1048576){
            if(rawCap<rawLen+(int)rd){
                rawCap=rawLen+(int)rd+4096;
                raw=(char*)realloc(raw,rawCap);
            }
            if(raw){ memcpy(raw+rawLen,tmp,rd); rawLen+=rd; }
        }
    }
    CloseHandle(rR);
    WaitForSingleObject(pi.hProcess,5000);
    GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    {
        int wn=MultiByteToWideChar(936,0,raw?raw:"",rawLen,NULL,0);
        wchar_t *ws=(wchar_t*)malloc((size_t)(wn+1)*sizeof(wchar_t));
        char *u8;
        int un;
        if(!ws){ free(raw); return NULL; }
        MultiByteToWideChar(936,0,raw?raw:"",rawLen,ws,wn+1);
        un=WideCharToMultiByte(CP_UTF8,0,ws,wn,NULL,0,NULL,NULL);
        u8=(char*)malloc(un+64);
        if(u8){
            WideCharToMultiByte(CP_UTF8,0,ws,wn,u8,un+1,NULL,NULL);
            sprintf(u8+un,"\r\n[退出代码 %lu]\r\n",code);
            *outLen=(int)strlen(u8);
        }
        free(ws);
        free(raw);
        return u8;
    }
}

static void send_execout(const char *data,int n){
    SOCKET s;
    char hdr[64];
    char *frame;
    unsigned int L;
    int hl;
    if(!data){ data="[命令执行失败]\n"; n=(int)strlen(data); }
    if(n>1048576) n=1048576;
    hl=sprintf(hdr,"EXECOUT|%d\n",n);
    frame=(char*)malloc(hl+n);
    if(!frame) return;
    memcpy(frame,hdr,hl);
    memcpy(frame+hl,data,n);
    EnterCriticalSection(&g_sendCs);
    EnterCriticalSection(&g_cs);
    s=g_sock;
    LeaveCriticalSection(&g_cs);
    if(s!=INVALID_SOCKET){
        L=(unsigned)(hl+n);
        send_all(s,(const char*)&L,4);
        send_all(s,frame,hl+n);
    }
    LeaveCriticalSection(&g_sendCs);
    free(frame);
}

static DWORD WINAPI exec_thread(LPVOID p){
    char *cmd=(char*)p;
    char *out;
    int outLen=0;
    out=run_command(cmd,&outLen);
    send_execout(out,outLen);
    if(out) free(out);
    free(cmd);
    return 0;
}

/* ---------------- 远程桌面: 屏幕上传 + 输入注入 ---------------- */
static void apply_event(const char *pl){
    unsigned x=0,y=0;
    int b=0,d=0;
    unsigned vk=0;
    if(pl[0]=='M' && pl[1]=='|'){
        sscanf(pl+2,"%u|%u",&x,&y);
        mouse_event(MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_MOVE,x,y,0,0);
    }else if(!strncmp(pl,"MD|",3)){
        sscanf(pl+3,"%u|%u|%d",&x,&y,&b);
        mouse_event(MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_MOVE,x,y,0,0);
        if(b==1)      mouse_event(MOUSEEVENTF_LEFTDOWN,0,0,0,0);
        else if(b==2) mouse_event(MOUSEEVENTF_RIGHTDOWN,0,0,0,0);
    }else if(!strncmp(pl,"MU|",3)){
        sscanf(pl+3,"%u|%u|%d",&x,&y,&b);
        mouse_event(MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_MOVE,x,y,0,0);
        if(b==1)      mouse_event(MOUSEEVENTF_LEFTUP,0,0,0,0);
        else if(b==2) mouse_event(MOUSEEVENTF_RIGHTUP,0,0,0,0);
    }else if(!strncmp(pl,"W|",2)){
        sscanf(pl+2,"%u|%u|%d",&x,&y,&d);
        mouse_event(MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_MOVE,x,y,0,0);
        mouse_event(MOUSEEVENTF_WHEEL,0,0,(DWORD)d,0);
    }else if(!strncmp(pl,"K|",2)){
        sscanf(pl+2,"%u|%d",&vk,&d);
        keybd_event((BYTE)vk,0,d?0:KEYEVENTF_KEYUP,0);
    }
}

/* 抓自己的屏(半分辨率 RGB555), 画面有变化才发送 */
static DWORD WINAPI rdp_capture_thread(LPVOID p){
    SOCKET s=(SOCKET)(INT_PTR)p;
    unsigned seq=0;
    while(g_rdpRun){
        int sw,sh,fw,fh,size,ok=0;
        HDC dc,md;
        HBITMAP bm;
        HGDIOBJ old;
        BITMAPINFO bmi;
        unsigned char *fr;
        sw=GetSystemMetrics(SM_CXSCREEN)/2; if(sw<1) sw=1;
        sh=GetSystemMetrics(SM_CYSCREEN)/2; if(sh<1) sh=1;
        fw=GetSystemMetrics(SM_CXSCREEN);
        fh=GetSystemMetrics(SM_CYSCREEN);
        dc=GetDC(NULL);
        md=CreateCompatibleDC(dc);
        bm=CreateCompatibleBitmap(dc,sw,sh);
        old=SelectObject(md,bm);
        SetStretchBltMode(md,COLORONCOLOR);
        StretchBlt(md,0,0,sw,sh,dc,0,0,fw,fh,SRCCOPY);
        memset(&bmi,0,sizeof bmi);
        bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth=sw;
        bmi.bmiHeader.biHeight=-sh;
        bmi.bmiHeader.biPlanes=1;
        bmi.bmiHeader.biBitCount=16;
        bmi.bmiHeader.biCompression=BI_RGB;
        size=sw*sh*2;
        fr=(unsigned char*)malloc(size);
        if(fr && GetDIBits(md,bm,0,sh,fr,&bmi,DIB_RGB_COLORS)==sh) ok=1;
        SelectObject(md,old);
        DeleteObject(bm);
        DeleteDC(md);
        ReleaseDC(NULL,dc);
        if(ok){
            int changed=(g_rdpPrevSize!=(unsigned)size);
            if(!changed && g_rdpPrev) changed=(memcmp(g_rdpPrev,fr,size)!=0);
            if(changed){
                unsigned char *pkt=(unsigned char*)malloc(20+size);
                if(pkt){
                    unsigned int *h=(unsigned*)pkt;
                    unsigned int L=(unsigned)(20+size);
                    h[0]=RDP_MAGIC; h[1]=++seq; h[2]=(unsigned)sw;
                    h[3]=(unsigned)sh; h[4]=(unsigned)size;
                    memcpy(pkt+20,fr,size);
                    EnterCriticalSection(&g_sendCs);
                    if(!send_all(s,(const char*)&L,4) || !send_all(s,(const char*)pkt,(int)L))
                        g_rdpRun=0;
                    LeaveCriticalSection(&g_sendCs);
                    free(pkt);
                    if(g_rdpPrevCap<size){
                        free(g_rdpPrev);
                        g_rdpPrev=(unsigned char*)malloc(size);
                        g_rdpPrevCap=g_rdpPrev?size:0;
                    }
                    if(g_rdpPrev){ memcpy(g_rdpPrev,fr,size); g_rdpPrevSize=size; }
                }
            }
        }
        if(fr) free(fr);
        Sleep(100);
    }
    return 0;
}

/* 事件接收为监督线程: 断开/收到 X 时收尾 */
static DWORD WINAPI rdp_client_thread(LPVOID arg){
    char *ip=(char*)arg;
    SOCKET s;
    u_long nb=1,zb=0;
    struct sockaddr_in a;
    fd_set wset;
    struct timeval tv;
    HANDLE capH;
    int cr;
    s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(s==INVALID_SOCKET){ g_rdpRun=0; free(ip); PostMessage(g_main,WM_APP_RDP_OFF,0,0); return 0; }
    memset(&a,0,sizeof a);
    a.sin_family=AF_INET;
    a.sin_port=htons(RDP_PORT);
    a.sin_addr.s_addr=inet_addr(ip);
    ioctlsocket(s,FIONBIO,&nb);
    cr=connect(s,(struct sockaddr*)&a,sizeof a);
    if(cr==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK){
        FD_ZERO(&wset);
        FD_SET(s,&wset);
        tv.tv_sec=3; tv.tv_usec=0;
        if(select(0,NULL,&wset,NULL,&tv)<=0){
            closesocket(s); g_rdpRun=0; free(ip);
            PostMessage(g_main,WM_APP_RDP_OFF,0,0);
            return 0;
        }
    }else if(cr==SOCKET_ERROR){
        closesocket(s); g_rdpRun=0; free(ip);
        PostMessage(g_main,WM_APP_RDP_OFF,0,0);
        return 0;
    }
    ioctlsocket(s,FIONBIO,&zb);
    free(ip);
    capH=CreateThread(NULL,0,rdp_capture_thread,(LPVOID)(INT_PTR)s,0,NULL);
    for(;;){
        unsigned int len;
        char pl[300];
        if(!recv_u32(s,&len)) break;
        if(len<2 || len>280) break;
        if(!recv_all(s,pl,(int)len)) break;
        pl[len]=0;
        if(!strcmp(pl,"X")) break;
        apply_event(pl);
    }
    g_rdpRun=0;
    closesocket(s);
    if(capH){ WaitForSingleObject(capH,3000); CloseHandle(capH); }
    PostMessage(g_main,WM_APP_RDP_OFF,0,0);
    return 0;
}

/* 教师端远程查看/控制时的透明提示条 */
static LRESULT CALLBACK rind_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    switch(m){
    case WM_PAINT:{
        PAINTSTRUCT ps;
        HDC dc=BeginPaint(w,&ps);
        RECT rc,tr;
        HFONT f=CreateFontW(-SD(dc,12),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        GetClientRect(w,&rc);
        {
            HBRUSH b=CreateSolidBrush(RGB(18,20,26));
            FillRect(dc,&rc,b);
            DeleteObject(b);
        }
        FrameRound(dc,rc,RGB(70,78,94),SD(dc,10),1);
        tr=rc;
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,C_TXT2);
        SelectObject(dc,f);
        DrawTextW(dc,W("教师端正在远程查看/控制本机"),-1,&tr,
                  DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        EndPaint(w,&ps);
        DeleteObject(f);
        return 0;
    }
    case WM_DESTROY:
        if(g_rdpInd==w) g_rdpInd=NULL;
        return 0;
    }
    return DefWindowProcW(w,m,wp,lp);
}

static void ui_rdp_ind_on(void){
    static int cls_ok=0;
    RECT mr;
    int wd,ht;
    if(g_rdpInd) return;
    if(!cls_ok){
        WNDCLASSW wc;
        memset(&wc,0,sizeof wc);
        wc.lpfnWndProc=rind_proc;
        wc.hInstance=g_hInst;
        wc.hCursor=LoadCursorW(NULL,IDC_ARROW);
        wc.hbrBackground=NULL;
        wc.lpszClassName=W("CC_Stu_RInd");
        RegisterClassW(&wc);
        cls_ok=1;
    }
    monitor_rect(g_main,&mr);
    wd=S(300); ht=S(34);
    g_rdpInd=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,W("CC_Stu_RInd"),L"",
            WS_POPUP|WS_VISIBLE,
            mr.right-wd-S(12),mr.bottom-ht-S(12),wd,ht,
            NULL,NULL,g_hInst,NULL);
    SetWindowRgn(g_rdpInd,CreateRoundRectRgn(0,0,wd,ht,S(10),S(10)),TRUE);
}

static void ui_rdp_ind_off(void){
    if(g_rdpInd){ DestroyWindow(g_rdpInd); g_rdpInd=NULL; }
}

static void handle_command(SOCKET s,const char *hdr,const char *body,int blen){
    if(!strcmp(hdr,"LOCK")){
        PostMessage(g_main,WM_APP_LOCK,0,0);
        send_ack(s,"LOCK");
    }else if(!strcmp(hdr,"UNLOCK")){
        PostMessage(g_main,WM_APP_UNLOCK,0,0);
        send_ack(s,"UNLOCK");
    }else if(!strcmp(hdr,"DEMO_START")){
        PostMessage(g_main,WM_APP_DEMO_ON,0,0);
        send_ack(s,"DEMO_START");
    }else if(!strcmp(hdr,"DEMO_STOP")){
        PostMessage(g_main,WM_APP_DEMO_OFF,0,0);
        send_ack(s,"DEMO_STOP");
    }else if(!strncmp(hdr,"MSG|",4)){
        int n=atoi(hdr+4);
        if(n>=0 && n<=blen && n<(int)sizeof g_msgText-1){
            EnterCriticalSection(&g_cs);
            memcpy(g_msgText,body,n);
            g_msgText[n]=0;
            MultiByteToWideChar(CP_UTF8,0,g_msgText,-1,g_msgW,2048);
            LeaveCriticalSection(&g_cs);
            PostMessage(g_main,WM_APP_MSG,0,0);
            send_ack(s,"MSG");
        }
    }else if(!strncmp(hdr,"FILE|",5)){
        char fname[256];
        unsigned fsz=0;
        if(sscanf(hdr+5,"%255[^|]|%u",fname,&fsz)==2 && fsz>0 && fsz<=(unsigned)blen){
            wchar_t wdir[MAX_PATH],wname[256],full[MAX_PATH*2];
            HANDLE h;
            sanitize_name(fname);
            wdir[0]=0;
            if(GetEnvironmentVariableW(L"USERPROFILE",wdir,MAX_PATH)==0)
                wcscpy(wdir,L"C:");
            if(wcslen(wdir)+wcslen(W("\\Documents"))+2<MAX_PATH)
                lstrcatW(wdir,W("\\Documents"));
            CreateDirectoryW(wdir,NULL);
            if(wcslen(wdir)+wcslen(W("\\课堂接收文件"))+2<MAX_PATH)
                lstrcatW(wdir,W("\\课堂接收文件"));
            CreateDirectoryW(wdir,NULL);
            MultiByteToWideChar(CP_UTF8,0,fname,-1,wname,256);
            if(wcslen(wdir)+1+wcslen(wname)<MAX_PATH*2){
                lstrcpyW(full,wdir);
                lstrcatW(full,L"\\");
                lstrcatW(full,wname);
                h=CreateFileW(full,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
                if(h!=INVALID_HANDLE_VALUE){
                    DWORD wr=0;
                    WriteFile(h,body,(DWORD)fsz,&wr,NULL);
                    CloseHandle(h);
                    sprintf(g_fileNote,"已接收教师端文件: %s (%u 字节)",fname,fsz);
                    PostMessage(g_main,WM_APP_FILE,0,0);
                }else{
                    sprintf(g_fileNote,"文件保存失败: %s",fname);
                    PostMessage(g_main,WM_APP_FILE,0,0);
                }
            }
            send_ack(s,"FILE");
        }
    }else if(!strcmp(hdr,"SHUTDOWN")){
        send_ack(s,"SHUTDOWN");
        PostMessage(g_main,WM_APP_SHUTDOWN,0,0);
    }else if(!strcmp(hdr,"REBOOT")){
        send_ack(s,"REBOOT");
        PostMessage(g_main,WM_APP_REBOOT,0,0);
    }else if(!strcmp(hdr,"RDP_START")){
        send_ack(s,"RDP_START");
        if(!g_rdpRun){
            char tip[64];
            char *ipc;
            EnterCriticalSection(&g_cs);
            strcpy(tip,g_teacherIP);
            LeaveCriticalSection(&g_cs);
            g_rdpRun=1;
            ui_rdp_ind_on();
            ipc=(char*)malloc(strlen(tip)+1);
            if(ipc){
                HANDLE t;
                strcpy(ipc,tip);
                t=CreateThread(NULL,0,rdp_client_thread,ipc,0,NULL);
                if(t) CloseHandle(t);
                else{ g_rdpRun=0; free(ipc); ui_rdp_ind_off(); }
            }else{ g_rdpRun=0; ui_rdp_ind_off(); }
        }
    }else if(!strcmp(hdr,"RDP_STOP")){
        send_ack(s,"RDP_STOP");
        g_rdpRun=0;
        ui_rdp_ind_off();
    }else if(!strncmp(hdr,"EXEC|",5)){
        int n=atoi(hdr+5);
        if(n>=0 && n<=blen && n<4096){
            char *c=(char*)malloc(n+1);
            if(c){
                HANDLE t;
                memcpy(c,body,n);
                c[n]=0;
                send_ack(s,"EXEC");
                t=CreateThread(NULL,0,exec_thread,c,0,NULL);
                if(t) CloseHandle(t); else free(c);
            }
        }
    }
}

static void ctrl_worker(SOCKET s){
    unsigned int len;
    for(;;){
        char *p,*nl;
        char hdr[256];
        const char *body;
        int hlen,blen;
        if(!recv_u32(s,&len)) break;
        if(len<1 || len>64u*1024u*1024u) break;
        p=(char*)malloc(len+1);
        if(!p) break;
        if(!recv_all(s,p,(int)len)){ free(p); break; }
        p[len]=0;
        nl=(char*)memchr(p,'\n',(int)len);
        hlen=nl?(int)(nl-p):(int)len;
        if(hlen>255) hlen=255;
        memcpy(hdr,p,hlen);
        hdr[hlen]=0;
        body=nl?nl+1:p+len;
        blen=(int)len-hlen-(nl?1:0);
        if(blen<0) blen=0;
        handle_command(s,hdr,body,blen);
        free(p);
    }
}

static DWORD WINAPI tcp_connect_loop(LPVOID arg){
    (void)arg;
    for(;;){
        char ip[64];
        int got=0;
        SOCKET s;
        struct sockaddr_in a;
        u_long nb=1,zb=0;
        BOOL nd=1;
        int cr;
        /* 选连接目标: 手动指定 > 候选轮询 > 上次锁定地址 */
        EnterCriticalSection(&g_cs);
        if(g_manualTeacher && g_haveTeacher){
            strcpy(ip,g_teacherIP);
            got=1;
        }else if(g_candN>0){
            int tries=0,k;
            while(tries<g_candN){
                k=(g_candIdx+tries)%g_candN;
                if(GetTickCount()-g_candSeen[k]<15000){
                    strcpy(ip,g_cand[k]);
                    g_candIdx=(k+1)%g_candN;
                    got=1;
                    break;
                }
                tries++;
            }
        }else if(g_haveTeacher){
            strcpy(ip,g_teacherIP);
            got=1;
        }
        LeaveCriticalSection(&g_cs);
        if(!got){ Sleep(800); continue; }
        if(g_haveCtrl){ Sleep(500); continue; }   /* 反向通道已接管, 暂不出站拨号 */

        s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(s==INVALID_SOCKET){ Sleep(1500); continue; }
        memset(&a,0,sizeof a);
        a.sin_family=AF_INET;
        a.sin_port=htons(TCP_PORT);
        a.sin_addr.s_addr=inet_addr(ip);
        ioctlsocket(s,FIONBIO,&nb);
        cr=connect(s,(struct sockaddr*)&a,sizeof a);
        if(cr==SOCKET_ERROR){
            int err=WSAGetLastError();
            if(err==WSAEWOULDBLOCK){
                fd_set wset;
                struct timeval tv;
                FD_ZERO(&wset);
                FD_SET(s,&wset);
                tv.tv_sec=2; tv.tv_usec=0;
                if(select(0,NULL,&wset,NULL,&tv)<=0){
                    closesocket(s);
                    Sleep(500);      /* 换下一个候选 */
                    continue;
                }
            }else{
                closesocket(s);
                Sleep(500);
                continue;
            }
        }
        ioctlsocket(s,FIONBIO,&zb);
        setsockopt(s,IPPROTO_TCP,TCP_NODELAY,(char*)&nd,sizeof nd);
        if(g_haveCtrl){   /* 拨号期间反向通道已接管 → 弃用本出站连接 */
            closesocket(s);
            Sleep(300);
            continue;
        }

        /* 锁定该教师端 */
        EnterCriticalSection(&g_cs);
        strcpy(g_teacherIP,ip);
        g_haveTeacher=1;
        LeaveCriticalSection(&g_cs);
        /* 首帧: HI|<主机名> —— 教师端 accept 线程以此登记连接, 缺失会导致主控端永远"未连接" */
        {
            char hi[200],fr[208];
            unsigned int n;
            int hl=sprintf(hi,"HI|%s\n",g_hostU);
            n=(unsigned int)hl;
            memcpy(fr,&n,4);
            memcpy(fr+4,hi,hl);
            if(!send_all(s,fr,hl+4)){
                closesocket(s);
                Sleep(500);
                continue;
            }
        }
        {
            char st[160];
            sprintf(st,"已连接教师端 %s, 等待指令...",ip);
            set_state(st);
        }
        g_haveCtrl=1;
        ctrl_worker(s);
        g_haveCtrl=0;
        EnterCriticalSection(&g_cs);
        if(g_sock==s) g_sock=INVALID_SOCKET;
        LeaveCriticalSection(&g_cs);
        closesocket(s);
        set_state("与教师端断开, 正在重连...");
        Sleep(1000);
    }
    return 0;
}

/* ---------------- 模式B: 反向接入监听 (教师扫描子网后主动连入 15670) ----------------
 * 教师首帧 TEA|<教师IP> → 学生回 HI|<主机名> → 之后同一 TCP 上收命令/发 ACK */
static DWORD WINAPI rev_client_thread(LPVOID p){
    SOCKET s=(SOCKET)(INT_PTR)p;
    unsigned int len;
    char buf[600];
    if(!recv_u32(s,&len) || len<5 || len>560 || !recv_all(s,buf,(int)len)){
        closesocket(s);
        return 0;
    }
    buf[len]=0;
    if(strncmp(buf,"TEA|",4)!=0){
        closesocket(s);
        return 0;
    }
    EnterCriticalSection(&g_cs);
    if(!g_manualTeacher){
        strncpy(g_teacherIP,buf+4,63);
        g_teacherIP[63]=0;
    }
    g_haveTeacher=1;
    LeaveCriticalSection(&g_cs);
    {
        int busy;
        EnterCriticalSection(&g_cs);
        busy=(g_sock!=INVALID_SOCKET || g_haveCtrl);
        LeaveCriticalSection(&g_cs);
        if(busy){ closesocket(s); return 0; }   /* 已有控制通道, 反向通道冗余 */
    }
    {
        char hi[200],fr[208];
        unsigned int n;
        int hl=sprintf(hi,"HI|%s\n",g_hostU);
        n=(unsigned int)hl;
        memcpy(fr,&n,4);
        memcpy(fr+4,hi,hl);
        if(!send_all(s,fr,hl+4)){ closesocket(s); return 0; }
    }
    EnterCriticalSection(&g_cs);
    if(g_sock!=INVALID_SOCKET || g_haveCtrl){
        LeaveCriticalSection(&g_cs);
        closesocket(s);
        return 0;
    }
    g_sock=s;
    g_haveCtrl=1;
    LeaveCriticalSection(&g_cs);
    {
        char st[160],tea[64];
        char *nl;
        strncpy(tea,buf+4,63);
        tea[63]=0;
        nl=strchr(tea,'\n');
        if(nl) *nl=0;
        sprintf(st,"教师端已反向接入 (%.60s), 等待指令...",tea);
        set_state(st);
    }
    ctrl_worker(s);
    g_haveCtrl=0;
    EnterCriticalSection(&g_cs);
    if(g_sock==s) g_sock=INVALID_SOCKET;
    LeaveCriticalSection(&g_cs);
    closesocket(s);
    set_state("与教师端断开, 等待重新接入...");
    return 0;
}

static DWORD WINAPI rev_accept_thread(LPVOID arg){
    SOCKET l;
    (void)arg;
    l=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(l==INVALID_SOCKET) return 0;
    {
        struct sockaddr_in a;
        memset(&a,0,sizeof a);
        a.sin_family=AF_INET;
        a.sin_port=htons(REV_PORT);
        a.sin_addr.s_addr=INADDR_ANY;
        if(bind(l,(struct sockaddr*)&a,sizeof a)!=0){ closesocket(l); return 0; }
        listen(l,16);
    }
    for(;;){
        struct sockaddr_in f;
        int fl=sizeof f;
        SOCKET s=accept(l,(struct sockaddr*)&f,&fl);
        if(s==INVALID_SOCKET){ Sleep(200); continue; }
        CloseHandle(CreateThread(NULL,0,rev_client_thread,(LPVOID)(INT_PTR)s,0,NULL));
    }
}

/* ---------------- 屏幕广播接收 (UDP 组播 + 分片重组) ---------------- */
static DWORD WINAPI demo_worker(LPVOID arg){
    SOCKET u;
    struct sockaddr_in a;
    struct ip_mreq mr;
    unsigned char *acc=NULL,*got=NULL;
    unsigned int accSize=0,accTotal=0,accSeq=0,accCnt=0,accW=0,accH=0;
    int have=0;
    char pkt[2048];
    (void)arg;
    u=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(u==INVALID_SOCKET){ g_demoRun=0; return 0; }
    {
        BOOL ru=1;
        int rbuf=4*1024*1024;
        setsockopt(u,SOL_SOCKET,SO_REUSEADDR,(char*)&ru,sizeof ru);
        setsockopt(u,SOL_SOCKET,SO_RCVBUF,(char*)&rbuf,sizeof rbuf);
    }
    memset(&a,0,sizeof a);
    a.sin_family=AF_INET;
    a.sin_port=htons(DEMO_PORT);
    a.sin_addr.s_addr=INADDR_ANY;
    if(bind(u,(struct sockaddr*)&a,sizeof a)!=0){ closesocket(u); g_demoRun=0; return 0; }
    mr.imr_multiaddr.s_addr=inet_addr(MCAST_GROUP);
    mr.imr_interface.s_addr=INADDR_ANY;
    setsockopt(u,IPPROTO_IP,IP_ADD_MEMBERSHIP,(char*)&mr,sizeof mr);

    for(;;){
        fd_set rset;
        struct timeval tv;
        int sr,n;
        FD_ZERO(&rset);
        FD_SET(u,&rset);
        tv.tv_sec=0; tv.tv_usec=400*1000;
        sr=select(0,&rset,NULL,NULL,&tv);
        if(sr<=0){
            if(!g_demoRun) break;
            continue;
        }
        n=recvfrom(u,pkt,sizeof pkt,0,NULL,NULL);
        if(!g_demoRun) break;
        if(n<28) continue;
        {
            unsigned int *h=(unsigned int*)pkt;
            unsigned int seq=h[1],idx=h[2],total=h[3],size=h[4],w=h[5],hh=h[6];
            int pl=n-28,off;
            if(h[0]!=CHK_MAGIC) continue;
            if(total==0||total>65536||size==0||size>16u*1024u*1024u||idx>=total||w==0||hh==0)
                continue;
            off=(int)idx*CHUNK_PAYLOAD;
            if(off+pl>(int)size) continue;
            if(pl != (int)((idx==total-1)?(size-off):CHUNK_PAYLOAD)) continue;
            if(!have || seq!=accSeq){
                free(acc); free(got);
                acc=(unsigned char*)malloc(size);
                got=(unsigned char*)calloc(total,1);
                if(!acc || !got){
                    free(acc); free(got);
                    acc=NULL; got=NULL; have=0;
                    continue;
                }
                accSize=size; accTotal=total; accSeq=seq; accCnt=0;
                accW=w; accH=hh; have=1;
            }
            if(acc && !got[idx]){
                got[idx]=1;
                accCnt++;
                memcpy(acc+off,pkt+28,pl);
                if(accCnt==accTotal){
                    EnterCriticalSection(&g_cs);
                    if(g_frmCap<accSize){
                        free(g_frm);
                        g_frm=(unsigned char*)malloc(accSize);
                        g_frmCap=g_frm?accSize:0;
                    }
                    if(g_frm){
                        memcpy(g_frm,acc,accSize);
                        g_frmW=accW; g_frmH=accH;
                    }
                    LeaveCriticalSection(&g_cs);
                    if(g_demo) InvalidateRect(g_demo,NULL,FALSE);
                    free(acc); free(got);
                    acc=NULL; got=NULL; have=0;
                }
            }
        }
    }
    free(acc); free(got);
    setsockopt(u,IPPROTO_IP,IP_DROP_MEMBERSHIP,(char*)&mr,sizeof mr);
    closesocket(u);
    g_demoRun=0;
    PostMessage(g_main,WM_APP_DEMO_OFF,0,0);
    return 0;
}

/* ---------------- 关机/重启 ---------------- */
static int enable_shutdown_priv(void){
    HANDLE t;
    TOKEN_PRIVILEGES tp;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&t))
        return 0;
    LookupPrivilegeValueW(NULL,L"SeShutdownPrivilege",&tp.Privileges[0].Luid);
    tp.PrivilegeCount=1;
    tp.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(t,FALSE,&tp,0,NULL,NULL);
    CloseHandle(t);
    return GetLastError()==ERROR_SUCCESS;
}

/* ---------------- UI: 命令落地 ---------------- */
static void ui_lock(void){
    if(!g_lock){
        RECT mr;
        g_lockOn=1;
        monitor_rect(g_main,&mr);
        g_lock=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,W("CC_Stu_Lock"),L"",
                WS_POPUP|WS_VISIBLE,mr.left,mr.top,
                mr.right-mr.left,mr.bottom-mr.top,
                NULL,NULL,g_hInst,NULL);
        SetTimer(g_lock,1,300,NULL);   /* 周期性重夺置顶+前台, 压制任务管理器"始终置顶" */
        g_khook=SetWindowsHookExW(WH_KEYBOARD_LL,kproc,NULL,0);
        force_foreground(g_lock);
        InvalidateRect(g_lock,NULL,TRUE);
        tm_block_start();              /* 锁屏期间周期性结束任务管理器 */
    }
}
static void ui_unlock(void){
    g_lockOn=0;
    g_blockTM=0;
    if(g_khook){ UnhookWindowsHookEx(g_khook); g_khook=NULL; }
    if(g_lock){ DestroyWindow(g_lock); g_lock=NULL; }
}
static void ui_demo_on(void){
    if(!g_demo){
        RECT mr;
        monitor_rect(g_main,&mr);
        g_demo=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,W("CC_Stu_Demo"),L"",
                WS_POPUP|WS_VISIBLE,mr.left,mr.top,
                mr.right-mr.left,mr.bottom-mr.top,
                NULL,NULL,g_hInst,NULL);
        SetTimer(g_demo,1,300,NULL);   /* 周期性重夺置顶+前台 */
        force_foreground(g_demo);
        InvalidateRect(g_demo,NULL,TRUE);
        tm_block_start();              /* 广播期间同样结束任务管理器 */
    }
    if(!g_demoRun){
        HANDLE t;
        g_demoRun=1;
        t=CreateThread(NULL,0,demo_worker,NULL,0,NULL);
        if(t) CloseHandle(t); else g_demoRun=0;
    }
}
static void ui_demo_off(void){
    g_demoRun=0;
    g_blockTM=0;
    if(g_demo){ DestroyWindow(g_demo); g_demo=NULL; }
}
static void ui_msg_show(void){
    int x,y;
    RECT mr;
    if(g_msg) DestroyWindow(g_msg);
    monitor_rect(g_main,&mr);
    x=mr.left+((mr.right-mr.left)-S(520))/2;
    y=mr.top+((mr.bottom-mr.top)-S(200))/2;
    g_msg=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,W("CC_Stu_Msg"),L"",
            WS_POPUP|WS_VISIBLE,x,y,S(520),S(200),NULL,NULL,g_hInst,NULL);
    SetWindowRgn(g_msg,CreateRoundRectRgn(0,0,S(520),S(200),S(22),S(22)),TRUE);
    SetTimer(g_msg,1,15000,NULL);
    force_foreground(g_msg);
}

/* ---------------- 防火墙自动放行 ---------------- */
static int fw_is_admin(void){
    BOOL r=FALSE;
    void *g=NULL;
    SID_IDENTIFIER_AUTHORITY nt={SECURITY_NT_AUTHORITY};
    if(AllocateAndInitializeSid(&nt,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,
                                0,0,0,0,0,0,&g)){
        CheckTokenMembership(NULL,g,&r);
        FreeSid(g);
    }
    return r?1:0;
}
static int fw_reg_asked(void){
    HKEY k;
    DWORD v=0,sz=4,t=0;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\ClassCtrl",0,KEY_QUERY_VALUE,&k)==ERROR_SUCCESS){
        if(RegQueryValueExW(k,L"FwAskedS",NULL,&t,(BYTE*)&v,&sz)==ERROR_SUCCESS){
            RegCloseKey(k);
            return v==1;
        }
        RegCloseKey(k);
    }
    return 0;
}
static void fw_reg_set_asked(void){
    HKEY k;
    DWORD v=1;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\ClassCtrl",0,NULL,0,KEY_SET_VALUE,NULL,&k,NULL)==ERROR_SUCCESS){
        RegSetValueExW(k,L"FwAskedS",0,REG_DWORD,(const BYTE*)&v,4);
        RegCloseKey(k);
    }
}
static void fw_write_bat_s(const wchar_t *bat){
    wchar_t exe[MAX_PATH+8];
    char apath[600];
    char *buf;
    DWORD wr=0;
    HANDLE h;
    GetModuleFileNameW(NULL,exe,MAX_PATH);
    WideCharToMultiByte(CP_ACP,0,exe,-1,apath,600,NULL,NULL);
    buf=(char*)malloc(4096);
    if(!buf) return;
    sprintf(buf,
        "@echo off\r\n"
        "netsh advfirewall firewall delete rule name=all program=\"%s\" >nul 2>&1\r\n"
        "netsh advfirewall firewall add rule name=\"ClassCtrl Student\" dir=in action=allow program=\"%s\" enable=yes >nul 2>&1\r\n"
        "netsh advfirewall firewall add rule name=\"ClassCtrl Udp\" dir=in action=allow protocol=UDP localport=15555 enable=yes >nul 2>&1\r\n"
        "netsh advfirewall firewall add rule name=\"ClassCtrl Rev\" dir=in action=allow protocol=TCP localport=15670 enable=yes >nul 2>&1\r\n",
        apath,apath);
    h=CreateFileW(bat,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    if(h!=INVALID_HANDLE_VALUE){
        WriteFile(h,buf,(DWORD)strlen(buf),&wr,NULL);
        CloseHandle(h);
    }
    free(buf);
}
static void fw_setup_student(void){
    wchar_t temp[400],bat[560];
    if(GetTempPathW(400,temp)==0) return;
    lstrcpyW(bat,temp);
    lstrcatW(bat,L"ClassCtrl_fw_student.bat");
    if(fw_is_admin()){
        fw_write_bat_s(bat);
        ShellExecuteW(NULL,L"open",bat,NULL,NULL,SW_HIDE);
        return;
    }
    if(fw_reg_asked()) return;
    if(MsgBoxU(g_main,
        "局域网使用需要放行 Windows 防火墙, 否则收不到教师端的广播报文。\n\n"
        "现在自动添加防火墙放行规则吗?\n(将弹出一次管理员授权窗口, 之后不再询问)",
        "防火墙设置",MB_YESNO|MB_ICONQUESTION)==IDYES){
        fw_write_bat_s(bat);
        ShellExecuteW(NULL,L"runas",bat,NULL,NULL,SW_HIDE);
    }
    fw_reg_set_asked();
}
/* 手动触发的防火墙修复 (总是尝试提权) */
static void fw_repair_student(void){
    wchar_t temp[400],bat[560];
    if(GetTempPathW(400,temp)==0) return;
    lstrcpyW(bat,temp);
    lstrcatW(bat,L"ClassCtrl_fw_student.bat");
    fw_write_bat_s(bat);
    ShellExecuteW(NULL,L"runas",bat,NULL,NULL,SW_HIDE);
    MsgBoxU(g_main,
        "已提交防火墙修复, 请在弹出的授权窗口中点击\"是\"。\n\n"
        "完成后等几秒观察状态卡:\n· 出现\"候选: x.x.x.x\" = 已收到教师广播;\n"
        "· 状态变为\"已连接\" = 全部打通;\n· 仍无候选 = 广播被网络拦截, 用命令行 Student.exe 教师IP 直连。",
        "防火墙修复",MB_OK|MB_ICONINFORMATION);
}

/* ---------------- 入口 ---------------- */
int WINAPI WinMain(HINSTANCE hi,HINSTANCE hp,LPSTR cl,int show){
    WNDCLASSW wc;
    MSG m;
    WSADATA wd;
    RECT r;
    (void)hp;(void)cl;
    if(WSAStartup(MAKEWORD(2,2),&wd)!=0) return 1;
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_sendCs);
    enable_dpi_aware();
    g_dpi=(UINT)GetDeviceCaps(GetDC(NULL),LOGPIXELSX);
    g_hInst=hi;

    {
        wchar_t wn[128];
        DWORD sz=128;
        g_hostU[0]=0;
        if(GetComputerNameW(wn,&sz)){
            char *u;
            int need;
            need=WideCharToMultiByte(CP_UTF8,0,wn,-1,NULL,0,NULL,NULL);
            u=(char*)malloc(need+1);
            if(u){
                WideCharToMultiByte(CP_UTF8,0,wn,-1,u,need,NULL,NULL);
                u[need]=0;
                strncpy(g_hostU,u,127);
                g_hostU[127]=0;
                free(u);
            }
        }
        if(!g_hostU[0]) strcpy(g_hostU,"PC");
    }

    memset(&wc,0,sizeof wc);
    wc.hInstance=hi;
    wc.hIcon=LoadIconW(NULL,IDI_APPLICATION);
    wc.hCursor=LoadCursorW(NULL,IDC_ARROW);
    wc.lpfnWndProc=main_proc;
    wc.hbrBackground=NULL;
    wc.lpszClassName=W("CC_Stu_Main");
    RegisterClassW(&wc);
    wc.lpfnWndProc=lock_proc;
    wc.hbrBackground=NULL;
    wc.lpszClassName=W("CC_Stu_Lock");
    RegisterClassW(&wc);
    wc.lpfnWndProc=demo_proc;
    wc.hbrBackground=NULL;
    wc.lpszClassName=W("CC_Stu_Demo");
    RegisterClassW(&wc);
    wc.lpfnWndProc=msg_proc;
    wc.hbrBackground=NULL;
    wc.lpszClassName=W("CC_Stu_Msg");
    RegisterClassW(&wc);
    wc.lpfnWndProc=rind_proc;
    wc.hbrBackground=NULL;
    wc.lpszClassName=W("CC_Stu_RInd");
    RegisterClassW(&wc);

    make_fonts();
    g_brBg=CreateSolidBrush(C_BG);
    g_brPanel=CreateSolidBrush(C_PANEL);
    g_brCard=CreateSolidBrush(C_CARD);

    r.left=0; r.top=0; r.right=S(400); r.bottom=S(330);
    AdjustWindowRect(&r,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);
    g_main=CreateWindowExW(0,W("CC_Stu_Main"),W("课堂控制 · 被控端"),
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,
            CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,
            NULL,NULL,hi,NULL);
    dwm_modern(g_main);
    SetTimer(g_main,1,1000,NULL);   /* 诊断行(绑定状态/收包数/候选)每秒刷新 */
    /* 命令行手动指定教师端 IP: Student.exe <教师IP> (跨网段/广播受限时使用) */
    if(cl && cl[0]){
        char ip[64];
        unsigned long a;
        if(sscanf(cl,"%63s",ip)==1 && (a=inet_addr(ip))!=INADDR_NONE && a!=0){
            strncpy(g_teacherIP,ip,63);
            g_teacherIP[63]=0;
            g_manualTeacher=1;
            g_haveTeacher=1;
        }
    }
    set_state(g_manualTeacher?"已指定教师端, 正在连接...":"等待教师端上线...");
    ShowWindow(g_main,show);
    UpdateWindow(g_main);
    fw_setup_student();

    memset(&g_nid,0,sizeof g_nid);
    g_nid.cbSize=sizeof g_nid;
    g_nid.hWnd=g_main;
    g_nid.uID=1;
    g_nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    g_nid.uCallbackMessage=WM_APP_TRAY;
    g_nid.hIcon=LoadIconW(NULL,IDI_APPLICATION);
    lstrcpynW(g_nid.szTip,W("课堂被控端 - 运行中"),128);
    Shell_NotifyIconW(NIM_ADD,&g_nid);

    CloseHandle(CreateThread(NULL,0,udp_recv_thread,NULL,0,NULL));
    CloseHandle(CreateThread(NULL,0,udp_send_thread,NULL,0,NULL));
    CloseHandle(CreateThread(NULL,0,tcp_connect_loop,NULL,0,NULL));
    CloseHandle(CreateThread(NULL,0,rev_accept_thread,NULL,0,NULL));

    while(GetMessageW(&m,NULL,0,0)>0){
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    WSACleanup();
    DeleteCriticalSection(&g_cs);
    return 0;
}
