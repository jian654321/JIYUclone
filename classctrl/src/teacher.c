/* =========================================================================
 * teacher.c - 课堂控制主控端(教师端) v1.2
 * 仿极域电子教室架构的教学演示实现。仅用于学习/课堂教学, 请勿用于非法用途。
 *
 * v1.2: 修复提示文字重叠; 全程 Per-Monitor DPI 感知(物理像素渲染, 高清);
 *       克制的深色设计(中性按钮+彩色圆点), 卡片列表空状态提示, 输入框占位符。
 *
 * 线程模型 (对应极域教师端的 截图/编码/发送 流水线):
 *   主线程      : Win32 界面
 *   beacon 线程 : 每 3 秒 UDP 广播教师端存在 (学生据此发现教师)
 *   udp 线程    : 接收学生 STULOGIN/STUHB 报文, 维护学生登记表
 *   accept 线程 : 接受学生 TCP 控制连接, 每连接一个回执线程
 *   capture 线程: 截屏(缩放一半, RGB555) -> 拆 1400B 小片 -> UDP 组播发送
 * ========================================================================= */
#define _WIN32_WINNT 0x0600
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dwmapi.h>
#include <iphlpapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define APP_TITLE "课堂控制 · 主控端 v1.5.0"

/* 控件 ID */
#define ID_LB         101
#define ID_EDIT       102
#define ID_BTN_MSG    110
#define ID_BTN_LOCK   111
#define ID_BTN_UNLOCK 112
#define ID_BTN_DEMO1  113
#define ID_BTN_DEMO0  114
#define ID_BTN_FILE   115
#define ID_BTN_SHUT   116
#define ID_BTN_REBOOT 117
#define ID_BTN_RDP    118
#define ID_BTN_EXEC   119
#define ID_BTN_FW     121
#define ID_BTN_ADDIP  122

#define WM_APP_REFRESH (WM_APP+1)
#define WM_APP_EXEC_REFRESH (WM_APP+9)
#define WCLASS_MAIN "CC_Teacher_Main"
#define WCLASS_EXEC "CC_T_Exec"
#define WCLASS_RDP  "CC_T_Rdp"

/* 远程终端对话框控件 */
#define ID_EXEC_OUT  1200
#define ID_EXEC_IN   1201
#define ID_EXEC_RUN  1202
#define ID_EXEC_CLR  1203

#define MAXSTU  256

/* ---------------- 配色 ---------------- */
#define C_BG     RGB(24,26,32)
#define C_PANEL  RGB(32,35,43)
#define C_CARD   RGB(42,46,56)
#define C_CARDH  RGB(54,60,72)
#define C_CARDSL RGB(50,58,74)
#define C_BAR    RGB(17,19,24)
#define C_DOT    RGB(84,142,250)
#define C_DOTG   RGB(76,203,124)
#define C_DOTR   RGB(242,92,84)
#define C_DOTP   RGB(155,115,248)
#define C_DOTN   RGB(140,148,162)
#define C_TXT    RGB(232,236,244)
#define C_TXT2   RGB(140,148,162)
#define C_TXT3   RGB(108,114,126)

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

/* ---------------- DPI 缩放 ---------------- */
static UINT g_dpi=96;
static int S(int v){ return MulDiv(v,(int)g_dpi,96); }

static void enable_dpi_aware(void){
    typedef BOOL (WINAPI *FnCtx)(HANDLE);
    HMODULE u=GetModuleHandleW(L"user32.dll");
    FnCtx f=(FnCtx)(void*)GetProcAddress(u,"SetProcessDpiAwarenessContext");
    if(f){
        if(!f((HANDLE)-4))   /* PER_MONITOR_AWARE_V2 */
            if(!f((HANDLE)-3)) f((HANDLE)-2);   /* per-monitor / system */
    }else{
        SetProcessDPIAware();
    }
}

/* ---------------- GDI 绘图小工具 ---------------- */
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
/* 在 (x,y) 画单行文字 (矩形只作承载, 不参与换行) */
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
/* 在矩形内画宽字符文本 (由调用方决定换行/对齐) */
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
typedef struct {
    char   ip[64];
    char   name[128];        /* UTF-8 */
    SOCKET sock;             /* 控制通道 TCP, INVALID_SOCKET=未连接 */
    int    locked;
    int    demo;
    DWORD  lastUDP;          /* 最近一次收到该学生 UDP 心跳的时间 */
} STU;

static STU  g_stu[MAXSTU];
static int  g_nStu;
static CRITICAL_SECTION g_cs;
static HINSTANCE g_hInst;
static HWND  g_main, g_lb;
static volatile LONG g_demoOn = 0;
static char  g_localIP[64];
/* 命令行指定的额外扫描网段 (Teacher.exe 192.168.32.0 ...) */
static char  g_extraSub[16][32];
static int   g_nExtra=0;
static int  g_map[MAXSTU];          /* 列表行号 -> 学生下标 */
static int  g_rowN=0;
static int  g_onlN=0;

/* 远程终端 */
static HWND g_execWnd,g_execOut,g_execIn;
static int  g_execIdx=-1;
static char g_execIp[64],g_execName[128];
static char g_execBuf[1048576+8];
static int  g_execLen=0;
static WNDPROC g_execInOrig;

/* 远程桌面 */
static HWND g_rdpWnd;
static SOCKET g_rdpSock=INVALID_SOCKET;
static int g_rdpActive=0;
static unsigned char *g_rdpFrm=NULL;
static unsigned g_rdpW=0,g_rdpH=0,g_rdpCap=0;
static int g_rdpIdx=-1;
static char g_rdpIp[64],g_rdpName[128];
static CRITICAL_SECTION g_rdpSendCs;

static HFONT g_fTitle,g_fSub,g_fBody,g_fBtn,g_fName,g_fPill,g_fMono;
static HBRUSH g_brBg,g_brPanel,g_brCard;

/* 前置声明 */
static int g_tgt[MAXSTU];
static int build_targets(void);
static void dwm_modern(HWND w);
static void fw_repair(void);
static DWORD WINAPI ctrl_client_thread(LPVOID arg);

/* 学生登记表查找/添加 (需持有 g_cs 调用) */
static int find_stu(const char *ip){
    int i;
    for(i=0;i<g_nStu;i++)
        if(!strcmp(g_stu[i].ip,ip)) return i;
    return -1;
}
static int find_or_add(const char *ip){
    int i=find_stu(ip);
    if(i>=0) return i;
    if(g_nStu<MAXSTU){
        i=g_nStu++;
        memset(&g_stu[i],0,sizeof(STU));
        strcpy(g_stu[i].ip,ip);
        g_stu[i].sock=INVALID_SOCKET;
        return i;
    }
    return -1;
}

static void make_fonts(void){
    if(g_fTitle) DeleteObject(g_fTitle);
    if(g_fSub)   DeleteObject(g_fSub);
    if(g_fBody)  DeleteObject(g_fBody);
    if(g_fBtn)   DeleteObject(g_fBtn);
    if(g_fName)  DeleteObject(g_fName);
    if(g_fPill)  DeleteObject(g_fPill);
    if(g_fMono)  DeleteObject(g_fMono);
    g_fTitle=CreateFontW(-S(21),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    g_fSub=CreateFontW(-S(13),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    g_fBody=CreateFontW(-S(15),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    g_fBtn=CreateFontW(-S(15),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    g_fName=CreateFontW(-S(15),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    g_fPill=CreateFontW(-S(12),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    g_fMono=CreateFontW(-S(13),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                       0,0,CLEARTYPE_QUALITY,0,L"Consolas");
}

/* 教师端: 控制帧发送 */
static int send_frame(SOCKET s,const char *data,int len){
    unsigned int n=(unsigned int)len;
    if(!send_all(s,(const char*)&n,4)) return 0;
    return send_all(s,data,len);
}
static int send_cmd(SOCKET s,const char *cmd){
    char b[128];
    int n=sprintf(b,"%s\n",cmd);
    return send_frame(s,b,n);
}

static SOCKET sock_of(int i){
    SOCKET s=INVALID_SOCKET;
    EnterCriticalSection(&g_cs);
    if(i>=0 && i<g_nStu) s=g_stu[i].sock;
    LeaveCriticalSection(&g_cs);
    return s;
}

/* 判断目标 IP 是否是本机地址 (同机场景: 走 127.0.0.1 回环拨号最可靠) */
static int is_local_ip(const char *ip){
    ULONG sz=16*1024,ret,want;
    IP_ADAPTER_ADDRESSES *ad=NULL,*a;
    int r=0;
    want=inet_addr(ip);
    if(want==INADDR_NONE || want==INADDR_ANY) return 0;
    for(;;){
        ad=(IP_ADAPTER_ADDRESSES*)malloc(sz);
        if(!ad) return 0;
        ret=GetAdaptersAddresses(AF_INET,
              GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,
              NULL,ad,&sz);
        if(ret==NO_ERROR) break;
        free(ad); ad=NULL;
        if(ret!=ERROR_BUFFER_OVERFLOW) return 0;
        sz*=2;
    }
    for(a=ad;a && !r;a=a->Next){
        if(!a->FirstUnicastAddress ||
           a->FirstUnicastAddress->Address.lpSockaddr->sa_family!=AF_INET) continue;
        {
            struct sockaddr_in *u4=(struct sockaddr_in*)a->FirstUnicastAddress->Address.lpSockaddr;
            if(u4->sin_addr.s_addr==want) r=1;
        }
    }
    free(ad);
    return r;
}

/* 按需拨号: 学生心跳证明其可达时, 教师可随时主动连其 REV_PORT 建立控制通道 */
static SOCKET dial_student(const char *ip){
    SOCKET s;
    struct sockaddr_in a;
    u_long nb=1,zb=0;
    fd_set wset;
    struct timeval tv;
    int cr;
    unsigned int len;
    char buf[600],myip[64],target[64];
    myip[0]=0;
    strcpy(target,ip);
    if(is_local_ip(ip)) strcpy(target,"127.0.0.1");   /* 同机: 走回环, 避免 IP 不匹配被 RST */
    s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(s==INVALID_SOCKET) return INVALID_SOCKET;
    memset(&a,0,sizeof a);
    a.sin_family=AF_INET;
    a.sin_port=htons(REV_PORT);
    a.sin_addr.s_addr=inet_addr(target);
    if(a.sin_addr.s_addr==INADDR_NONE){ closesocket(s); return INVALID_SOCKET; }
    ioctlsocket(s,FIONBIO,&nb);
    cr=connect(s,(struct sockaddr*)&a,sizeof a);
    if(cr==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK){
        FD_ZERO(&wset);
        FD_SET(s,&wset);
        tv.tv_sec=2; tv.tv_usec=0;
        if(select(0,NULL,&wset,NULL,&tv)<=0){
            closesocket(s);
            return INVALID_SOCKET;
        }
    }else if(cr==SOCKET_ERROR){
        closesocket(s);
        return INVALID_SOCKET;
    }
    ioctlsocket(s,FIONBIO,&zb);
    {
        struct sockaddr_in la;
        int ln=sizeof la;
        if(getsockname(s,(struct sockaddr*)&la,&ln)==0 && la.sin_family==AF_INET){
            strncpy(myip,inet_ntoa(la.sin_addr),63);
            myip[63]=0;
        }
    }
    {
        char tea[80];
        unsigned int L;
        int hl;
        if(!myip[0]) strcpy(myip,ip);
        hl=sprintf(tea,"TEA|%s\n",myip);
        L=(unsigned)hl;
        if(!send_all(s,(const char*)&L,4) || !send_all(s,tea,hl)){
            closesocket(s);
            return INVALID_SOCKET;
        }
    }
    if(!recv_u32(s,&len) || len<4 || len>560 || !recv_all(s,buf,(int)len)){
        closesocket(s);
        return INVALID_SOCKET;
    }
    buf[len]=0;
    if(strncmp(buf,"HI|",3)!=0){
        closesocket(s);
        return INVALID_SOCKET;
    }
    {
        char *nm=buf+3,*q;
        for(q=nm; *q; q++)
            if((unsigned char)*q<32 || *q=='|' || *q==';') *q='_';
        nm=buf+3;
        EnterCriticalSection(&g_cs);
        {
            int i=find_or_add(ip);
            if(i>=0){
                if(nm[0]){ strncpy(g_stu[i].name,nm,127); g_stu[i].name[127]=0; }
                g_stu[i].sock=s;
            }
        }
        LeaveCriticalSection(&g_cs);
    }
    {
        char *ipc=(char*)malloc(strlen(ip)+1);
        if(ipc){
            HANDLE t;
            strcpy(ipc,ip);
            t=CreateThread(NULL,0,ctrl_client_thread,ipc,0,NULL);
            if(t) CloseHandle(t); else free(ipc);
        }
    }
    PostMessage(g_main,WM_APP_REFRESH,0,0);
    return s;
}

/* 取学生控制通道: 有通道直接用; 无通道但 8 秒内心跳新鲜则按需拨号 */
static SOCKET ensure_sock(int i){
    SOCKET s=sock_of(i);
    char ip[64];
    DWORD lu=0;
    if(s!=INVALID_SOCKET) return s;
    if(i<0 || i>=g_nStu) return INVALID_SOCKET;
    EnterCriticalSection(&g_cs);
    strcpy(ip,g_stu[i].ip);
    lu=g_stu[i].lastUDP;
    LeaveCriticalSection(&g_cs);
    if(GetTickCount()-lu<8000)
        return dial_student(ip);
    return INVALID_SOCKET;
}

/* ---------------- UDP 发现通道 ---------------- */
static DWORD WINAPI udp_thread(LPVOID arg){
    SOCKET u;
    (void)arg;
    u=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(u==INVALID_SOCKET) return 0;
    {
        BOOL nb=1,ru=1;
        setsockopt(u,SOL_SOCKET,SO_REUSEADDR,(char*)&ru,sizeof ru);
        (void)nb;
    }
    {
        struct sockaddr_in a;
        memset(&a,0,sizeof a);
        a.sin_family=AF_INET;
        a.sin_port=htons(STU_UDP_PORT);   /* 只绑定学生上报端口, 15555 留给学生独占 */
        a.sin_addr.s_addr=INADDR_ANY;
        if(bind(u,(struct sockaddr*)&a,sizeof a)!=0){ closesocket(u); return 0; }
    }
    for(;;){
        char buf[512];
        struct sockaddr_in f;
        int fl=sizeof f;
        int n=recvfrom(u,buf,sizeof buf-1,0,(struct sockaddr*)&f,&fl);
        if(n<=0){ Sleep(200); continue; }
        buf[n]=0;
        if(!strncmp(buf,"STULOGIN|",9) || !strncmp(buf,"STUHB|",6)){
            char *nm=strchr(buf,'|')+1,*q;
            char ip[64];
            strcpy(ip,inet_ntoa(f.sin_addr));
            for(q=nm; *q; q++)
                if((unsigned char)*q<32 || *q=='|' || *q==';') *q='_';
            nm=strchr(buf,'|')+1;
            EnterCriticalSection(&g_cs);
            {
                int i=-1,k;
                for(k=0;k<g_nStu;k++)
                    if(!strcmp(g_stu[k].ip,ip)){ i=k; break; }
                if(i<0 && g_nStu<MAXSTU){
                    i=g_nStu++;
                    memset(&g_stu[i],0,sizeof(STU));
                    strcpy(g_stu[i].ip,ip);
                    g_stu[i].sock=INVALID_SOCKET;
                }
                if(i>=0){
                    strncpy(g_stu[i].name,nm,127);
                    g_stu[i].name[127]=0;
                    g_stu[i].lastUDP=GetTickCount();
                }
            }
            LeaveCriticalSection(&g_cs);
            PostMessage(g_main,WM_APP_REFRESH,0,0);
        }
    }
}

/* 发送一条 TEACON。bindip 非 NULL 时绑定源地址(子网定向广播需指定出口网卡);
 * 不绑定时 Windows 会把广播回环到本机监听 socket(同机测试必需) */
static void beacon_send(const char *pkt,int n,const char *dstip,const char *bindip){
    SOCKET u;
    struct sockaddr_in a,sa;
    BOOL nb=1;
    u=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(u==INVALID_SOCKET) return;
    setsockopt(u,SOL_SOCKET,SO_BROADCAST,(char*)&nb,sizeof nb);
    if(bindip && bindip[0]){
        memset(&sa,0,sizeof sa);
        sa.sin_family=AF_INET;
        sa.sin_addr.s_addr=inet_addr(bindip);
        if(sa.sin_addr.s_addr!=INADDR_NONE && sa.sin_addr.s_addr!=INADDR_ANY)
            bind(u,(struct sockaddr*)&sa,sizeof sa);
    }
    memset(&a,0,sizeof a);
    a.sin_family=AF_INET;
    a.sin_port=htons(UDP_PORT);
    a.sin_addr.s_addr=inet_addr(dstip);
    if(a.sin_addr.s_addr!=INADDR_NONE)
        sendto(u,pkt,n,0,(struct sockaddr*)&a,sizeof a);
    closesocket(u);
}

/* 枚举所有活动网卡的 IPv4 地址, 向每个子网的定向广播地址各发一份
 * (255.255.255.255 只从默认网卡发出, 多网卡机器上其它网段收不到);
 * 报文携带对应网卡的 IP, 学生端才能锁定到真正可达的地址 */
static void beacon_subnet(void){
    ULONG sz=16*1024,ret;
    IP_ADAPTER_ADDRESSES *ad=NULL,*a;
    for(;;){
        ad=(IP_ADAPTER_ADDRESSES*)malloc(sz);
        if(!ad) return;
        ret=GetAdaptersAddresses(AF_INET,
              GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,
              NULL,ad,&sz);
        if(ret==NO_ERROR) break;
        free(ad);
        ad=NULL;
        if(ret!=ERROR_BUFFER_OVERFLOW) return;   /* 非缓冲区错误: 直接放弃, 严禁迭代未初始化数据 */
        sz*=2;
    }
    if(ad){
        char pkt[64];
        int n;
        for(a=ad;a;a=a->Next){
            if(a->OperStatus!=IfOperStatusUp) continue;
            if(a->IfType==IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if(!a->FirstUnicastAddress ||
               a->FirstUnicastAddress->Address.lpSockaddr->sa_family!=AF_INET) continue;
            {
                struct sockaddr_in *u4=(struct sockaddr_in*)a->FirstUnicastAddress->Address.lpSockaddr;
                ULONG ip=ntohl(u4->sin_addr.s_addr);
                ULONG bits=32,mask,bc;
                char bip[32],bcast[32];
                if(a->FirstPrefix) bits=(ULONG)a->FirstPrefix->PrefixLength;
                if(bits>32) bits=32;
                mask=bits? (0xFFFFFFFFu<<(32-bits)) : 0;
                bc=ip|((~mask)&0xFFFFFFFFu);
                sprintf(bip,"%u.%u.%u.%u",
                        (unsigned)((ip>>24)&255),(unsigned)((ip>>16)&255),
                        (unsigned)((ip>>8)&255),(unsigned)(ip&255));
                sprintf(bcast,"%u.%u.%u.%u",
                        (unsigned)((bc>>24)&255),(unsigned)((bc>>16)&255),
                        (unsigned)((bc>>8)&255),(unsigned)(bc&255));
                sprintf(pkt,"TEACON|v1|%s",bip);
                n=(int)strlen(pkt);
                beacon_send(pkt,n,bcast,bip);
            }
        }
        free(ad);
    }
}

/* 核心广播线程: 只做 全局+回环 两种必达发送 (独立于网卡枚举, 枚举崩溃也不影响) */
static DWORD WINAPI beacon_thread(LPVOID arg){
    char pkt[128];
    int n;
    (void)arg;
    for(;;){
        /* 1) 全局广播, 不绑定源地址 → 默认网卡发出, 且本机监听者可收到 */
        sprintf(pkt,"TEACON|v1|%s",g_localIP);
        n=(int)strlen(pkt);
        beacon_send(pkt,n,"255.255.255.255",NULL);
        /* 2) 本机回环单播兜底: 同机运行两端时必达 (绑定源地址的广播不回环) */
        sprintf(pkt,"TEACON|v1|127.0.0.1");
        n=(int)strlen(pkt);
        beacon_send(pkt,n,"127.0.0.1","127.0.0.1");
        Sleep(3000);
    }
    return 0;
}
/* 子网定向广播线程: 与核心广播隔离, 即使枚举异常也不拖垮核心广播 */
static DWORD WINAPI subnet_beacon_thread(LPVOID arg){
    (void)arg;
    for(;;){
        beacon_subnet();
        Sleep(3000);
    }
    return 0;
}

/* ---------------- TCP 控制通道 ---------------- */
static void exec_append(const char *data,int n){
    if(n<=0) return;
    if(n>1048576) n=1048576;
    EnterCriticalSection(&g_cs);
    if(g_execLen+n>900000){
        int drop=g_execLen+n-700000;
        if(drop>g_execLen) drop=g_execLen;
        if(drop>0){ memmove(g_execBuf,g_execBuf+drop,g_execLen-drop); g_execLen-=drop; }
    }
    memcpy(g_execBuf+g_execLen,data,n);
    g_execLen+=n;
    g_execBuf[g_execLen]=0;
    LeaveCriticalSection(&g_cs);
    if(g_execWnd) PostMessage(g_execWnd,WM_APP_EXEC_REFRESH,0,0);
}

/* 选中行中找第一个在线学生; 未选择则取第一个在线学生 */
static int pick_one_target(char *ipout,char *nameout){
    int n=build_targets(),k;
    for(k=0;k<n;k++){
        int i=g_tgt[k];
        if(i<0 || i>=g_nStu) continue;
        if(ensure_sock(i)==INVALID_SOCKET) continue;
        EnterCriticalSection(&g_cs);
        strcpy(ipout,g_stu[i].ip);
        strcpy(nameout,g_stu[i].name[0]?g_stu[i].name:"(未命名)");
        LeaveCriticalSection(&g_cs);
        return i;
    }
    return -1;
}

static DWORD WINAPI ctrl_client_thread(LPVOID arg){
    char *ip=(char*)arg;
    SOCKET my=INVALID_SOCKET;
    unsigned int len;
    EnterCriticalSection(&g_cs);
    {
        int i=-1,k;
        for(k=0;k<g_nStu;k++)
            if(!strcmp(g_stu[k].ip,ip)){ i=k; break; }
        if(i>=0) my=g_stu[i].sock;
    }
    LeaveCriticalSection(&g_cs);
    for(;;){
        char stackbuf[600];
        char *pb;
        int dyn=0;
        char hdr[256];
        char *nl;
        int hlen;
        if(!recv_u32(my,&len)) break;
        if(len<4 || len>1024u*1024u) break;
        if(len<sizeof stackbuf){ pb=stackbuf; }
        else { pb=(char*)malloc(len+1); if(!pb) break; dyn=1; }
        if(!recv_all(my,pb,(int)len)){ if(dyn) free(pb); break; }
        pb[len]=0;
        nl=(char*)memchr(pb,'\n',(int)len);
        hlen=nl?(int)(nl-pb):(int)len;
        if(hlen>255) hlen=255;
        memcpy(hdr,pb,hlen);
        hdr[hlen]=0;
        if(!strncmp(hdr,"ACK|",4)){
            const char *c=hdr+4;
            EnterCriticalSection(&g_cs);
            {
                int j=-1,k;
                for(k=0;k<g_nStu;k++)
                    if(!strcmp(g_stu[k].ip,ip)){ j=k; break; }
                if(j>=0){
                    if(!strcmp(c,"LOCK"))        g_stu[j].locked=1;
                    else if(!strcmp(c,"UNLOCK")) g_stu[j].locked=0;
                    else if(!strcmp(c,"DEMO_START")) g_stu[j].demo=1;
                    else if(!strcmp(c,"DEMO_STOP"))  g_stu[j].demo=0;
                }
            }
            LeaveCriticalSection(&g_cs);
            PostMessage(g_main,WM_APP_REFRESH,0,0);
        }else if(!strncmp(hdr,"EXECOUT|",8) && nl){
            int n=atoi(hdr+8);
            int blen=(int)len-hlen-1;
            int match=0;
            if(n>=0 && n<=blen){
                EnterCriticalSection(&g_cs);
                match=(g_execWnd!=NULL && strcmp(ip,g_execIp)==0);
                LeaveCriticalSection(&g_cs);
                if(match) exec_append(nl+1,n);
            }
        }
        if(dyn) free(pb);
    }
    EnterCriticalSection(&g_cs);
    {
        int j=-1,k;
        for(k=0;k<g_nStu;k++)
            if(!strcmp(g_stu[k].ip,ip)){ j=k; break; }
        if(j>=0 && g_stu[j].sock==my) g_stu[j].sock=INVALID_SOCKET;
    }
    LeaveCriticalSection(&g_cs);
    closesocket(my);
    free(ip);
    PostMessage(g_main,WM_APP_REFRESH,0,0);
    return 0;
}

static DWORD WINAPI accept_thread(LPVOID arg){
    SOCKET l;
    (void)arg;
    l=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(l==INVALID_SOCKET) return 0;
    {
        struct sockaddr_in a;
        memset(&a,0,sizeof a);
        a.sin_family=AF_INET;
        a.sin_port=htons(TCP_PORT);
        a.sin_addr.s_addr=INADDR_ANY;
        if(bind(l,(struct sockaddr*)&a,sizeof a)!=0){ closesocket(l); return 0; }
        listen(l,16);
    }
    for(;;){
        struct sockaddr_in f;
        int fl=sizeof f;
        SOCKET s=accept(l,(struct sockaddr*)&f,&fl);
        if(s==INVALID_SOCKET){ Sleep(200); continue; }
        {
            unsigned int len=0;
            char buf[600];
            char ip[64];
            int ok=0;
            strcpy(ip,inet_ntoa(f.sin_addr));
            if(recv_u32(s,&len) && len>=4 && len<560 && recv_all(s,buf,(int)len)){
                buf[len]=0;
                if(!strncmp(buf,"HI|",3)){
                    char *nm=buf+3,*q;
                    int dup=0;
                    for(q=nm; *q; q++)
                        if((unsigned char)*q<32 || *q=='|' || *q==';') *q='_';
                    EnterCriticalSection(&g_cs);
                    {
                        int i=-1,k;
                        for(k=0;k<g_nStu;k++)
                            if(!strcmp(g_stu[k].ip,ip)){ i=k; break; }
                        if(i<0 && g_nStu<MAXSTU){
                            i=g_nStu++;
                            memset(&g_stu[i],0,sizeof(STU));
                            strcpy(g_stu[i].ip,ip);
                            g_stu[i].sock=INVALID_SOCKET;
                        }
                        if(i>=0){
                            if(g_stu[i].sock!=INVALID_SOCKET){
                                dup=1;               /* 该学生已有活动控制通道, 弃用重复连接 */
                            }else{
                                if(nm[0]){ strncpy(g_stu[i].name,nm,127); g_stu[i].name[127]=0; }
                                g_stu[i].sock=s;
                            }
                        }
                    }
                    LeaveCriticalSection(&g_cs);
                    if(dup){ closesocket(s); }
                    else ok=1;
                }
            }
            if(ok){
                char *ipc=(char*)malloc(strlen(ip)+1);
                if(ipc){
                    HANDLE t;
                    strcpy(ipc,ip);
                    t=CreateThread(NULL,0,ctrl_client_thread,ipc,0,NULL);
                    if(t) CloseHandle(t); else { free(ipc); closesocket(s); }
                }
                PostMessage(g_main,WM_APP_REFRESH,0,0);
            }else{
                closesocket(s);
            }
        }
    }
}

/* ================= 模式B: 子网反向扫描 (教师主动连学生 REV_PORT) =================
 * 完全不依赖广播: 教师枚举本机每个网卡的 /24 网段, 并行探测 x.x.x.x:15670,
 * 连上后该 TCP 直接作为控制通道 (身份帧 TEA|教师IP → 学生回 HI|主机名 → 命令/ACK) */
typedef struct { SOCKET s; char ip[64]; char myip[64]; } SCANARG;

static DWORD WINAPI scan_client_thread(LPVOID p){
    SCANARG *g=(SCANARG*)p;
    unsigned int len;
    char buf[600];
    {
        char tea[80],myip[64];
        unsigned int L;
        int hl;
        struct sockaddr_in la;
        int ln=sizeof la;
        myip[0]=0;
        if(getsockname(g->s,(struct sockaddr*)&la,&ln)==0 && la.sin_family==AF_INET){
            strncpy(myip,inet_ntoa(la.sin_addr),63);
            myip[63]=0;
        }
        if(!myip[0] && g->myip[0]) strncpy(myip,g->myip,63);
        if(!myip[0]) strcpy(myip,"0.0.0.0");
        hl=sprintf(tea,"TEA|%s\n",myip);
        L=(unsigned)hl;
        if(!send_all(g->s,(const char*)&L,4) || !send_all(g->s,tea,hl)){
            closesocket(g->s); free(g); return 0;
        }
    }
    if(!recv_u32(g->s,&len) || len<4 || len>560 || !recv_all(g->s,buf,(int)len)){
        closesocket(g->s); free(g); return 0;
    }
    buf[len]=0;
    if(strncmp(buf,"HI|",3)==0){
        char *nm=buf+3,*q;
        for(q=nm; *q; q++)
            if((unsigned char)*q<32 || *q=='|' || *q==';') *q='_';
        nm=buf+3;
        EnterCriticalSection(&g_cs);
        {
            int i=find_or_add(g->ip);
            if(i>=0){
                if(nm[0]){ strncpy(g_stu[i].name,nm,127); g_stu[i].name[127]=0; }
                g_stu[i].sock=g->s;
            }
        }
        LeaveCriticalSection(&g_cs);
        {
            char *ipc=(char*)malloc(strlen(g->ip)+1);
            if(ipc){
                HANDLE t;
                strcpy(ipc,g->ip);
                t=CreateThread(NULL,0,ctrl_client_thread,ipc,0,NULL);
                if(t) CloseHandle(t); else free(ipc);
            }
        }
        PostMessage(g_main,WM_APP_REFRESH,0,0);
    }else{
        closesocket(g->s);
    }
    free(g);
    return 0;
}

/* 一批 (<=60) 非阻塞 connect, 1.2 秒内连上的交给 scan_client_thread */
static void scan_chunk(char (*ips)[64],int n,const char *bindip){
    SOCKET s[60];
    int i,pend=0;
    u_long nb=1;
    fd_set wset;
    struct timeval tv;
    FD_ZERO(&wset);
    if(n<=0) return;
    for(i=0;i<n;i++){
        struct sockaddr_in a,sa;
        s[i]=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(s[i]==INVALID_SOCKET) continue;
        memset(&sa,0,sizeof sa);
        sa.sin_family=AF_INET;
        sa.sin_addr.s_addr=inet_addr(bindip);
        if(sa.sin_addr.s_addr!=INADDR_NONE && sa.sin_addr.s_addr!=INADDR_ANY)
            bind(s[i],(struct sockaddr*)&sa,sizeof sa);
        ioctlsocket(s[i],FIONBIO,&nb);
        memset(&a,0,sizeof a);
        a.sin_family=AF_INET;
        a.sin_port=htons(REV_PORT);
        a.sin_addr.s_addr=inet_addr(ips[i]);
        if(connect(s[i],(struct sockaddr*)&a,sizeof a)==0){
            SCANARG *g=(SCANARG*)malloc(sizeof(SCANARG));
            if(g){
                g->s=s[i]; strcpy(g->ip,ips[i]); strcpy(g->myip,bindip);
                CloseHandle(CreateThread(NULL,0,scan_client_thread,g,0,NULL));
            }else closesocket(s[i]);
            s[i]=INVALID_SOCKET;
            continue;
        }
        if(WSAGetLastError()==WSAEWOULDBLOCK){
            FD_SET(s[i],&wset);
            pend++;
        }else{
            closesocket(s[i]);
            s[i]=INVALID_SOCKET;
        }
    }
    if(pend==0) return;
    tv.tv_sec=1; tv.tv_usec=200*1000;
    {
        int r=select(0,NULL,&wset,NULL,&tv);
        for(i=0;i<n;i++){
            if(s[i]==INVALID_SOCKET) continue;
            if(r>0 && FD_ISSET(s[i],&wset)){
                int err=0,elen=sizeof err;
                getsockopt(s[i],SOL_SOCKET,SO_ERROR,(char*)&err,&elen);
                if(err==0){
                    SCANARG *g=(SCANARG*)malloc(sizeof(SCANARG));
                    if(g){
                        g->s=s[i]; strcpy(g->ip,ips[i]); strcpy(g->myip,bindip);
                        CloseHandle(CreateThread(NULL,0,scan_client_thread,g,0,NULL));
                    }else closesocket(s[i]);
                    s[i]=INVALID_SOCKET;
                    continue;
                }
            }
            closesocket(s[i]);
            s[i]=INVALID_SOCKET;
        }
    }
}

static DWORD WINAPI scan_thread(LPVOID arg){
    (void)arg;
    for(;;){
        ULONG sz=16*1024,ret;
        IP_ADAPTER_ADDRESSES *ad=NULL,*a;
        for(;;){
            ad=(IP_ADAPTER_ADDRESSES*)malloc(sz);
            if(!ad) return 0;
            ret=GetAdaptersAddresses(AF_INET,
                  GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,
                  NULL,ad,&sz);
            if(ret==NO_ERROR) break;
            free(ad); ad=NULL;
            if(ret!=ERROR_BUFFER_OVERFLOW) break;
            sz*=2;
        }
        if(ad){
            for(a=ad;a;a=a->Next){
                if(a->OperStatus!=IfOperStatusUp) continue;
                if(a->IfType==IF_TYPE_SOFTWARE_LOOPBACK) continue;
                if(!a->FirstUnicastAddress ||
                   a->FirstUnicastAddress->Address.lpSockaddr->sa_family!=AF_INET) continue;
                {
                    struct sockaddr_in *u4=(struct sockaddr_in*)a->FirstUnicastAddress->Address.lpSockaddr;
                    ULONG ip=ntohl(u4->sin_addr.s_addr);
                    ULONG base=ip & 0xFFFFFF00u;
                    char bindip[32];
                    int h,n=0;
                    static char ips[60][64];
                    sprintf(bindip,"%u.%u.%u.%u",
                            (unsigned)((ip>>24)&255),(unsigned)((ip>>16)&255),
                            (unsigned)((ip>>8)&255),(unsigned)(ip&255));
                    for(h=1;h<=254;h++){
                        ULONG t=base|(ULONG)h;
                        char tip[32];
                        int skip=0;
                        if(t==ip) continue;   /* 自身 IP 不扫: 同机场景由 ensure_sock 走回环拨号 */
                        sprintf(tip,"%u.%u.%u.%u",
                                (unsigned)((t>>24)&255),(unsigned)((t>>16)&255),
                                (unsigned)((t>>8)&255),(unsigned)(t&255));
                        EnterCriticalSection(&g_cs);
                        {
                            int j=find_stu(tip);
                            if(j>=0 && g_stu[j].sock!=INVALID_SOCKET) skip=1;
                        }
                        LeaveCriticalSection(&g_cs);
                        if(skip) continue;
                        strcpy(ips[n],tip);
                        n++;
                        if(n==60){ scan_chunk(ips,n,bindip); n=0; }
                    }
                    scan_chunk(ips,n,bindip);
                }
            }
            free(ad);
        }
        /* 命令行指定的额外网段 (跨网段部署): Teacher.exe 192.168.32.0 ... */
        {
            int e,h;
            char ips2[60][64];
            for(e=0;e<g_nExtra;e++){
                ULONG ip=ntohl(inet_addr(g_extraSub[e]));
                ULONG base=ip & 0xFFFFFF00u;
                int n=0;
                if(g_extraSub[e][0]==0) continue;
                for(h=1;h<=254;h++){
                    ULONG t=base|(ULONG)h;
                    char tip[32];
                    int skip=0;
                    sprintf(tip,"%u.%u.%u.%u",
                            (unsigned)((t>>24)&255),(unsigned)((t>>16)&255),
                            (unsigned)((t>>8)&255),(unsigned)(t&255));
                    EnterCriticalSection(&g_cs);
                    {
                        int j=find_stu(tip);
                        if(j>=0 && g_stu[j].sock!=INVALID_SOCKET) skip=1;
                    }
                    LeaveCriticalSection(&g_cs);
                    if(skip) continue;
                    strcpy(ips2[n],tip);
                    n++;
                    if(n==60){ scan_chunk(ips2,n,NULL); n=0; }
                }
                scan_chunk(ips2,n,NULL);
            }
        }
        Sleep(10000);
    }
    return 0;
}

/* ================= 指定学生 IP 接入 (跨网段/任意可达地址) ================= */
#define ID_ADDIP_IN    1301
#define ID_ADDIP_OK    1302
#define ID_ADDIP_CANCEL 1303
static HWND g_addw=NULL,g_addEd=NULL;
static WNDPROC g_addEdOrig=NULL;

static LRESULT CALLBACK addip_in_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    if(m==WM_KEYDOWN){
        if(wp==VK_RETURN){
            SendMessageW(GetParent(w),WM_COMMAND,MAKELPARAM(ID_ADDIP_OK,0),(LPARAM)w);
            return 0;
        }
        if(wp==VK_ESCAPE){
            SendMessageW(GetParent(w),WM_COMMAND,MAKELPARAM(ID_ADDIP_CANCEL,0),(LPARAM)w);
            return 0;
        }
    }
    return CallWindowProcW(g_addEdOrig,w,m,wp,lp);
}

static void addip_do(void){
    wchar_t wt[128];
    char ip[128];
    int need;
    SOCKET s;
    GetDlgItemTextW(g_addw,1301,wt,128);
    need=WideCharToMultiByte(CP_UTF8,0,wt,-1,ip,128,NULL,NULL);
    if(need<=1){ return; }
    ip[need-1]=0;
    if(inet_addr(ip)==INADDR_NONE){
        MsgBoxU(g_addw,"IP 格式不正确 (示例: 192.168.32.35)","错误",MB_OK|MB_ICONERROR);
        return;
    }
    s=dial_student(ip);
    if(s!=INVALID_SOCKET){
        char msg[200];
        sprintf(msg,"已接入学生机 %s\n(该学生应已显示为在线)",ip);
        MsgBoxU(g_addw,msg,"指定学生IP",MB_OK|MB_ICONINFORMATION);
        DestroyWindow(g_addw);
    }else{
        MsgBoxU(g_addw,
            "接入失败。请检查:\n1. 学生机已运行 Student.exe 且防火墙放行(学生端\"防火墙修复\");\n2. 本机与该 IP 路由可达 (先 ping 测试);\n3. 跨网段时需 IT 开通路由, 或使用 VPN 组网。",
            "指定学生IP",MB_OK|MB_ICONERROR);
    }
}

static void draw_addip_btn(DRAWITEMSTRUCT *d){
    COLORREF c=(d->CtlID==ID_ADDIP_OK)?RGB(59,130,246):RGB(52,58,70);
    if(d->itemState & ODS_SELECTED) c=RGB(45,100,200);
    else if(d->itemState & ODS_HOTLIGHT) c=(d->CtlID==ID_ADDIP_OK)?RGB(90,150,250):RGB(54,60,72);
    FillRect(d->hDC,&d->rcItem,g_brBg);
    FillRound(d->hDC,d->rcItem,c,S(10));
    {
        HGDIOBJ of=SelectObject(d->hDC,g_fBtn);
        SetBkMode(d->hDC,TRANSPARENT);
        SetTextColor(d->hDC,RGB(255,255,255));
        DrawTextW(d->hDC,W(d->CtlID==ID_ADDIP_OK?"接 入":"取 消"),-1,&d->rcItem,
                  DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(d->hDC,of);
    }
}

static LRESULT CALLBACK addip_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    switch(m){
    case WM_CREATE:{
        HINSTANCE hi=(HINSTANCE)GetWindowLongPtrW(w,GWLP_HINSTANCE);
        HWND e=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
                S(24),S(46),S(292),S(24),w,(HMENU)(INT_PTR)ID_ADDIP_IN,hi,NULL);
        SendMessageW(e,WM_SETFONT,(WPARAM)g_fBody,MAKELPARAM(1,0));
        g_addEd=e;
        g_addEdOrig=(WNDPROC)SetWindowLongPtrW(e,GWLP_WNDPROC,(LONG_PTR)addip_in_proc);
        CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
                S(64),S(92),S(96),S(32),w,(HMENU)(INT_PTR)ID_ADDIP_OK,hi,NULL);
        CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
                S(180),S(92),S(96),S(32),w,(HMENU)(INT_PTR)ID_ADDIP_CANCEL,hi,NULL);
        SetFocus(e);
        return 0;
    }
    case WM_PAINT:{
        PAINTSTRUCT ps;
        HDC dc=BeginPaint(w,&ps);
        RECT rc;
        GetClientRect(w,&rc);
        {
            HBRUSH b=CreateSolidBrush(C_BG);
            FillRect(dc,&rc,b);
            DeleteObject(b);
        }
        txt(dc,S(24),S(16),"输入学生机 IP:",g_fBody,C_TXT);
        {
            RECT hr={S(24),S(120),S(336),S(170)};
            txtw(dc,hr,W("学生机需运行 Student.exe 并放行防火墙; 地址跨网段时需路由可达。"),
                 g_fPill,C_TXT3,DT_WORDBREAK);
        }
        EndPaint(w,&ps);
        return 0;
    }
    case WM_DRAWITEM:
        draw_addip_btn((DRAWITEMSTRUCT*)lp);
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
        if(LOWORD(wp)==ID_ADDIP_OK) addip_do();
        else if(LOWORD(wp)==ID_ADDIP_CANCEL) DestroyWindow(w);
        return 0;
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(g_addEd,GWLP_WNDPROC,(LONG_PTR)g_addEdOrig);
        g_addw=NULL;
        g_addEd=NULL;
        return 0;
    }
    return DefWindowProcW(w,m,wp,lp);
}

static void open_addip_dialog(void){
    static int cls_ok=0;
    RECT r;
    if(!cls_ok){
        WNDCLASSW wc;
        memset(&wc,0,sizeof wc);
        wc.lpfnWndProc=addip_proc;
        wc.hInstance=g_hInst;
        wc.hIcon=LoadIconW(NULL,IDI_APPLICATION);
        wc.hCursor=LoadCursorW(NULL,IDC_ARROW);
        wc.hbrBackground=NULL;
        wc.lpszClassName=W("CC_T_AddIp");
        RegisterClassW(&wc);
        cls_ok=1;
    }
    if(g_addw){ SetForegroundWindow(g_addw); return; }
    r.left=0; r.top=0; r.right=S(360); r.bottom=S(150);
    AdjustWindowRect(&r,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE);
    g_addw=CreateWindowExW(0,W("CC_T_AddIp"),W("指定学生IP"),
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
            CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,
            g_main,NULL,g_hInst,NULL);
    dwm_modern(g_addw);
    ShowWindow(g_addw,SW_SHOW);
    UpdateWindow(g_addw);
    SetFocus(g_addEd);
}

/* ---------------- 目标选择与命令下发 ---------------- */
static int build_targets(void){
    int n=0,sn;
    int sel[MAXSTU];
    sn=(int)SendMessageW(g_lb,LB_GETSELCOUNT,0,0);
    if(sn>0){
        int k;
        if(sn>MAXSTU) sn=MAXSTU;
        SendMessageW(g_lb,LB_GETSELITEMS,(WPARAM)sn,(LPARAM)sel);
        for(k=0;k<sn;k++){
            int li=sel[k];
            if(li>=0 && li<g_rowN) g_tgt[n++]=g_map[li];
        }
    }else{
        EnterCriticalSection(&g_cs);
        for(n=0;n<g_nStu && n<MAXSTU;n++) g_tgt[n]=n;
        LeaveCriticalSection(&g_cs);
    }
    return n;
}
static void do_cmd_all(const char *cmd){
    int n=build_targets(),tries=0,okc=0,k;
    for(k=0;k<n;k++){
        SOCKET s=ensure_sock(g_tgt[k]);
        if(s==INVALID_SOCKET) continue;
        tries++;
        if(send_cmd(s,cmd)) okc++;
    }
    if(tries==0)
        MsgBoxU(g_main,"没有已连接的学生。请先在被控端机器上运行 Student.exe。\n(未选中任何行时命令发给全部在线学生)","提示",MB_OK|MB_ICONINFORMATION);
    (void)okc;
}

/* ---------------- 广播消息 ---------------- */
static void send_msg_all(void){
    wchar_t wtxt[1024];
    int need,tl,hl,n,okc=0,tries=0,k;
    char *u8,*frame;
    char hdr[64];
    GetDlgItemTextW(g_main,ID_EDIT,wtxt,1024);
    need=WideCharToMultiByte(CP_UTF8,0,wtxt,-1,NULL,0,NULL,NULL);
    if(need<=1){ MsgBoxU(g_main,"请先输入要广播的消息内容","提示",MB_OK); return; }
    u8=(char*)malloc(need);
    if(!u8) return;
    WideCharToMultiByte(CP_UTF8,0,wtxt,-1,u8,need,NULL,NULL);
    tl=(int)strlen(u8);
    hl=sprintf(hdr,"MSG|%d\n",tl);
    frame=(char*)malloc(hl+tl);
    if(frame){
        memcpy(frame,hdr,hl);
        memcpy(frame+hl,u8,tl);
        n=build_targets();
        for(k=0;k<n;k++){
            SOCKET s=ensure_sock(g_tgt[k]);
            if(s==INVALID_SOCKET) continue;
            tries++;
            if(send_frame(s,frame,hl+tl)) okc++;
        }
        free(frame);
    }
    free(u8);
    if(tries==0)
        MsgBoxU(g_main,"没有已连接的学生","提示",MB_OK|MB_ICONINFORMATION);
    (void)okc;
}

/* ---------------- 下发文件 ---------------- */
static void send_file_all(void){
    wchar_t path[MAX_PATH+4];
    OPENFILENAMEW o;
    HANDLE h;
    DWORD hi=0,lo,rd,got=0;
    unsigned char *data,*frame;
    const wchar_t *base,*q;
    char nameU[512],hdr[600];
    int hl,n,okc=0,tries=0,k;
    path[0]=0;
    memset(&o,0,sizeof o);
    o.lStructSize=sizeof o;
    o.hwndOwner=g_main;
    o.lpstrFilter=L"Files (*.*)\0*.*\0\0";
    o.lpstrFile=path;
    o.nMaxFile=MAX_PATH;
    o.Flags=OFN_FILEMUSTEXIST|OFN_HIDEREADONLY;
    if(!GetOpenFileNameW(&o)) return;
    h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE){
        MsgBoxU(g_main,"无法打开所选文件","错误",MB_OK|MB_ICONERROR);
        return;
    }
    lo=GetFileSize(h,&hi);
    if(hi!=0 || lo>512u*1024u*1024u){
        CloseHandle(h);
        MsgBoxU(g_main,"文件过大(上限 512MB)","错误",MB_OK|MB_ICONERROR);
        return;
    }
    data=(unsigned char*)malloc(lo?lo:1);
    if(!data){ CloseHandle(h); return; }
    while(got<lo && ReadFile(h,data+got,lo-got,&rd,NULL) && rd) got+=rd;
    CloseHandle(h);
    if(got!=lo){
        free(data);
        MsgBoxU(g_main,"文件读取失败","错误",MB_OK|MB_ICONERROR);
        return;
    }
    base=path;
    for(q=path; *q; q++)
        if(*q==L'\\' || *q==L'/') base=q+1;
    WideCharToMultiByte(CP_UTF8,0,base,-1,nameU,512,NULL,NULL);
    hl=sprintf(hdr,"FILE|%s|%u\n",nameU,(unsigned)got);
    frame=(unsigned char*)malloc(hl+got);
    if(!frame){ free(data); return; }
    memcpy(frame,hdr,hl);
    memcpy(frame+hl,data,got);
    free(data);
    n=build_targets();
    for(k=0;k<n;k++){
        SOCKET s=ensure_sock(g_tgt[k]);
        if(s==INVALID_SOCKET) continue;
        tries++;
        if(send_frame(s,(const char*)frame,hl+(int)got)) okc++;
    }
    free(frame);
    {
        char msg[128];
        sprintf(msg,"下发完成: %d / %d 台成功\n(学生端保存于 文档\\课堂接收文件)",okc,tries);
        MsgBoxU(g_main,msg,"下发文件",MB_OK|MB_ICONINFORMATION);
    }
}

/* ---------------- 屏幕广播 (截屏 -> 拆片 -> UDP 组播) ---------------- */
static DWORD WINAPI capture_thread(LPVOID arg){
    SOCKET m;
    struct sockaddr_in ma;
    unsigned int seq=0;
    (void)arg;
    m=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(m==INVALID_SOCKET) return 0;
    {
        unsigned char ttl=8;
        int sbuf=4*1024*1024;
        setsockopt(m,IPPROTO_IP,IP_MULTICAST_TTL,(char*)&ttl,sizeof ttl);
        setsockopt(m,SOL_SOCKET,SO_SNDBUF,(char*)&sbuf,sizeof sbuf);
    }
    memset(&ma,0,sizeof ma);
    ma.sin_family=AF_INET;
    ma.sin_port=htons(DEMO_PORT);
    ma.sin_addr.s_addr=inet_addr(MCAST_GROUP);
    while(g_demoOn){
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
        if(fr){
            if(GetDIBits(md,bm,0,sh,fr,&bmi,DIB_RGB_COLORS)==sh) ok=1;
        }
        SelectObject(md,old);
        DeleteObject(bm);
        DeleteDC(md);
        ReleaseDC(NULL,dc);
        if(ok){
            unsigned int total=(unsigned)((size+CHUNK_PAYLOAD-1)/CHUNK_PAYLOAD);
            unsigned char pkt[CHUNK_PAYLOAD+28];
            unsigned int idx;
            seq++;
            for(idx=0; idx<total && g_demoOn; idx++){
                unsigned int *h=(unsigned int*)pkt;
                int off=(int)idx*CHUNK_PAYLOAD;
                int pl=size-off;
                if(pl>CHUNK_PAYLOAD) pl=CHUNK_PAYLOAD;
                h[0]=CHK_MAGIC; h[1]=seq; h[2]=idx; h[3]=total;
                h[4]=(unsigned)size; h[5]=(unsigned)sw; h[6]=(unsigned)sh;
                memcpy(pkt+28,fr+off,pl);
                sendto(m,(const char*)pkt,28+pl,0,(struct sockaddr*)&ma,sizeof ma);
            }
        }
        free(fr);
        Sleep(150);
    }
    closesocket(m);
    return 0;
}

static void demo_toggle(void){
    int n=build_targets(),k;
    if(!g_demoOn){
        g_demoOn=1;
        for(k=0;k<n;k++){
            SOCKET s=ensure_sock(g_tgt[k]);
            if(s!=INVALID_SOCKET) send_cmd(s,"DEMO_START");
        }
        CloseHandle(CreateThread(NULL,0,capture_thread,NULL,0,NULL));
        EnableWindow(GetDlgItem(g_main,ID_BTN_DEMO1),FALSE);
        EnableWindow(GetDlgItem(g_main,ID_BTN_DEMO0),TRUE);
        InvalidateRect(g_main,NULL,TRUE);
    }else{
        g_demoOn=0;
        for(k=0;k<n;k++){
            SOCKET s=ensure_sock(g_tgt[k]);
            if(s!=INVALID_SOCKET) send_cmd(s,"DEMO_STOP");
        }
        EnableWindow(GetDlgItem(g_main,ID_BTN_DEMO1),TRUE);
        EnableWindow(GetDlgItem(g_main,ID_BTN_DEMO0),FALSE);
        InvalidateRect(g_main,NULL,TRUE);
    }
}

/* ================= 远程终端 ================= */
static LRESULT CALLBACK exec_in_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    if(m==WM_KEYDOWN && wp==VK_RETURN){
        SendMessageW(GetParent(w),WM_COMMAND,MAKELPARAM(ID_EXEC_RUN,0),(LPARAM)w);
        return 0;
    }
    return CallWindowProcW(g_execInOrig,w,m,wp,lp);
}

static void refresh_exec_output(void){
    char *cp;
    wchar_t *ws;
    int wl,l;
    EnterCriticalSection(&g_cs);
    l=g_execLen;
    cp=(char*)malloc(l+1);
    if(cp){ memcpy(cp,g_execBuf,l); cp[l]=0; }
    LeaveCriticalSection(&g_cs);
    if(!cp) return;
    wl=MultiByteToWideChar(CP_UTF8,0,cp,l,NULL,0);
    ws=(wchar_t*)malloc((size_t)(wl+1)*sizeof(wchar_t));
    if(ws){
        MultiByteToWideChar(CP_UTF8,0,cp,l,ws,wl+1);
        ws[wl]=0;
        SetWindowTextW(g_execOut,ws);
        {
            int end=GetWindowTextLengthW(g_execOut);
            SendMessageW(g_execOut,EM_SETSEL,end,end);
            SendMessageW(g_execOut,EM_SCROLLCARET,0,0);
        }
        free(ws);
    }
    free(cp);
}

static void run_exec(void){
    wchar_t wt[2048];
    char *u8,*frame;
    char hdr[64],echo[2300];
    int need,tl,hl;
    unsigned int L;
    SOCKET s;
    GetWindowTextW(g_execIn,wt,2048);
    need=WideCharToMultiByte(CP_UTF8,0,wt,-1,NULL,0,NULL,NULL);
    if(need<=1) return;
    u8=(char*)malloc(need);
    if(!u8) return;
    WideCharToMultiByte(CP_UTF8,0,wt,-1,u8,need,NULL,NULL);
    tl=(int)strlen(u8);
    hl=sprintf(hdr,"EXEC|%d\n",tl);
    frame=(char*)malloc(hl+tl);
    if(!frame){ free(u8); return; }
    memcpy(frame,hdr,hl);
    memcpy(frame+hl,u8,tl);
    sprintf(echo,"\n> %s\n",u8);
    exec_append(echo,(int)strlen(echo));
    s=ensure_sock(g_execIdx);
    EnterCriticalSection(&g_rdpSendCs);
    L=(unsigned)(hl+tl);
    if(s==INVALID_SOCKET || !send_all(s,(const char*)&L,4) || !send_all(s,frame,hl+tl))
        exec_append("[发送失败: 学生端未连接]\n",30);
    LeaveCriticalSection(&g_rdpSendCs);
    free(frame);
    free(u8);
    SetWindowTextW(g_execIn,L"");
    SetFocus(g_execIn);
}

static void draw_exec_btn(DRAWITEMSTRUCT *d){
    COLORREF bg=C_CARD;
    const char *lb=(d->CtlID==ID_EXEC_RUN)?"执 行":"清 空";
    if(d->itemState & ODS_SELECTED) bg=RGB(34,38,46);
    else if(d->itemState & ODS_HOTLIGHT) bg=C_CARDH;
    FillRect(d->hDC,&d->rcItem,g_brBg);
    FillRound(d->hDC,d->rcItem,bg,S(10));
    {
        HGDIOBJ of=SelectObject(d->hDC,g_fBtn);
        SetBkMode(d->hDC,TRANSPARENT);
        SetTextColor(d->hDC,C_TXT);
        DrawTextW(d->hDC,W(lb),-1,&d->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(d->hDC,of);
    }
}

static LRESULT CALLBACK exec_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    switch(m){
    case WM_CREATE:{
        HINSTANCE hi=(HINSTANCE)GetWindowLongPtrW(w,GWLP_HINSTANCE);
        HWND e;
        g_execOut=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",
                WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
                S(16),S(52),S(728),S(348),w,(HMENU)(INT_PTR)ID_EXEC_OUT,hi,NULL);
        SendMessageW(g_execOut,WM_SETFONT,(WPARAM)g_fMono,MAKELPARAM(1,0));
        SendMessageW(g_execOut,EM_SETLIMITTEXT,2000000,0);
        e=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
                S(16),S(412),S(560),S(26),w,(HMENU)(INT_PTR)ID_EXEC_IN,hi,NULL);
        SendMessageW(e,WM_SETFONT,(WPARAM)g_fBody,MAKELPARAM(1,0));
        g_execIn=e;
        g_execInOrig=(WNDPROC)SetWindowLongPtrW(e,GWLP_WNDPROC,(LONG_PTR)exec_in_proc);
        CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
                S(592),S(410),S(76),S(32),w,(HMENU)(INT_PTR)ID_EXEC_RUN,hi,NULL);
        CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
                S(676),S(410),S(70),S(32),w,(HMENU)(INT_PTR)ID_EXEC_CLR,hi,NULL);
        SetFocus(e);
        return 0;
    }
    case WM_PAINT:{
        PAINTSTRUCT ps;
        HDC dc=BeginPaint(w,&ps);
        RECT rc;
        char hd[400];
        GetClientRect(w,&rc);
        {
            HBRUSH b=CreateSolidBrush(C_BG);
            FillRect(dc,&rc,b);
            DeleteObject(b);
        }
        sprintf(hd,"远程终端 · 目标: %s (%s) · 命令经 cmd.exe /C 在学生端执行, 超时 60 秒",
                g_execName,g_execIp);
        txt(dc,S(16),S(16),hd,g_fSub,C_TXT2);
        EndPaint(w,&ps);
        return 0;
    }
    case WM_DRAWITEM:
        draw_exec_btn((DRAWITEMSTRUCT*)lp);
        return TRUE;
    case WM_CTLCOLOREDIT:
        if(GetDlgCtrlID((HWND)lp)==ID_EXEC_OUT){
            SetTextColor((HDC)wp,RGB(150,220,160));
            SetBkColor((HDC)wp,RGB(16,18,23));
            return (LRESULT)(INT_PTR)CreateSolidBrush(RGB(16,18,23));
        }
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
        if(LOWORD(wp)==ID_EXEC_RUN) run_exec();
        else if(LOWORD(wp)==ID_EXEC_CLR){
            EnterCriticalSection(&g_cs);
            g_execLen=0;
            g_execBuf[0]=0;
            LeaveCriticalSection(&g_cs);
            refresh_exec_output();
            SetFocus(g_execIn);
        }
        return 0;
    case WM_APP_EXEC_REFRESH:
        refresh_exec_output();
        return 0;
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(g_execIn,GWLP_WNDPROC,(LONG_PTR)g_execInOrig);
        g_execWnd=NULL;
        g_execOut=NULL;
        g_execIn=NULL;
        return 0;
    }
    return DefWindowProcW(w,m,wp,lp);
}

static void open_exec_dialog(void){
    static int cls_ok=0;
    RECT r;
    int i;
    char title[300];
    if(!cls_ok){
        WNDCLASSW wc;
        memset(&wc,0,sizeof wc);
        wc.lpfnWndProc=exec_proc;
        wc.hInstance=g_hInst;
        wc.hIcon=LoadIconW(NULL,IDI_APPLICATION);
        wc.hCursor=LoadCursorW(NULL,IDC_ARROW);
        wc.hbrBackground=NULL;
        wc.lpszClassName=W("CC_T_Exec");
        RegisterClassW(&wc);
        cls_ok=1;
    }
    if(g_execWnd){ SetForegroundWindow(g_execWnd); return; }
    i=pick_one_target(g_execIp,g_execName);
    if(i<0){
        MsgBoxU(g_main,"没有已连接的学生\n(远程终端一次只针对一台机器, 选中多台时取第一台)","提示",MB_OK|MB_ICONINFORMATION);
        return;
    }
    g_execIdx=i;
    EnterCriticalSection(&g_cs);
    g_execLen=0;
    g_execBuf[0]=0;
    LeaveCriticalSection(&g_cs);
    r.left=0; r.top=0; r.right=S(760); r.bottom=S(480);
    AdjustWindowRect(&r,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);
    sprintf(title,"远程终端 - %s(%s)",g_execName,g_execIp);
    g_execWnd=CreateWindowExW(0,W("CC_T_Exec"),W(title),
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
            CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,
            g_main,NULL,g_hInst,NULL);
    dwm_modern(g_execWnd);
    ShowWindow(g_execWnd,SW_SHOW);
    UpdateWindow(g_execWnd);
}

/* ================= 远程桌面 ================= */
static void rdp_send(const char *line){
    SOCKET s;
    unsigned int n=(unsigned)strlen(line);
    char fr[300];
    if(n>280) return;
    memcpy(fr,&n,4);
    memcpy(fr+4,line,n);
    EnterCriticalSection(&g_rdpSendCs);
    s=g_rdpSock;
    if(s!=INVALID_SOCKET) send_all(s,fr,n+4);
    LeaveCriticalSection(&g_rdpSendCs);
}

/* 计算画面在客户区内的等比缩放矩形 */
static int rdp_dst_rect(RECT rc,RECT *dst){
    unsigned w,h;
    double sc;
    int dw,dh;
    EnterCriticalSection(&g_cs);
    w=g_rdpW; h=g_rdpH;
    LeaveCriticalSection(&g_cs);
    if(w==0||h==0) return 0;
    sc=((double)rc.right)/w;
    if(((double)rc.bottom)/h<sc) sc=((double)rc.bottom)/h;
    dw=(int)(w*sc); dh=(int)(h*sc);
    if(dw<1||dh<1) return 0;
    dst->left=(rc.right-dw)/2;
    dst->top=(rc.bottom-dh)/2;
    dst->right=dst->left+dw;
    dst->bottom=dst->top+dh;
    return 1;
}

static void rdp_mouse(HWND w,int x,int y,const char *ev){
    RECT rc,dst;
    UINT nx,ny;
    char line[96];
    GetClientRect(w,&rc);
    if(!rdp_dst_rect(rc,&dst)) return;
    if(x<dst.left) x=dst.left;
    if(x>dst.right) x=dst.right;
    if(y<dst.top) y=dst.top;
    if(y>dst.bottom) y=dst.bottom;
    nx=(UINT)((x-dst.left)*65535.0/(dst.right-dst.left));
    ny=(UINT)((y-dst.top)*65535.0/(dst.bottom-dst.top));
    sprintf(line,"%s|%u|%u",ev,nx,ny);
    rdp_send(line);
}

static void rdp_stop(void){
    SOCKET s;
    if(g_rdpIdx>=0){
        SOCKET c=ensure_sock(g_rdpIdx);
        if(c!=INVALID_SOCKET) send_cmd(c,"RDP_STOP");
    }
    EnterCriticalSection(&g_rdpSendCs);
    s=g_rdpSock;
    g_rdpSock=INVALID_SOCKET;
    g_rdpActive=0;
    LeaveCriticalSection(&g_rdpSendCs);
    if(s!=INVALID_SOCKET) closesocket(s);
}

static LRESULT CALLBACK rdp_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    switch(m){
    case WM_PAINT:{
        PAINTSTRUCT ps;
        HDC wdc=BeginPaint(w,&ps);
        HDC md=CreateCompatibleDC(wdc);
        HBITMAP mdbm=CreateCompatibleBitmap(wdc,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN));
        HGDIOBJ mdold=SelectObject(md,mdbm);
        HDC dc=md;
        RECT rc,dst;
        int have;
        GetClientRect(w,&rc);
        {
            HBRUSH b=CreateSolidBrush(RGB(0,0,0));
            FillRect(dc,&rc,b);
            DeleteObject(b);
        }
        have=rdp_dst_rect(rc,&dst);
        if(have){
            EnterCriticalSection(&g_cs);
            if(g_rdpFrm && g_rdpW && g_rdpH){
                BITMAPINFO bmi;
                memset(&bmi,0,sizeof bmi);
                bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth=(LONG)g_rdpW;
                bmi.bmiHeader.biHeight=-(LONG)g_rdpH;
                bmi.bmiHeader.biPlanes=1;
                bmi.bmiHeader.biBitCount=16;
                bmi.bmiHeader.biCompression=BI_RGB;
                SetStretchBltMode(dc,COLORONCOLOR);
                StretchDIBits(dc,dst.left,dst.top,dst.right-dst.left,dst.bottom-dst.top,
                              0,0,(int)g_rdpW,(int)g_rdpH,g_rdpFrm,&bmi,DIB_RGB_COLORS,SRCCOPY);
            }
            LeaveCriticalSection(&g_cs);
        }else{
            RECT tr=rc;
            HGDIOBJ of=SelectObject(dc,g_fBody);
            SetTextColor(dc,C_TXT2);
            SetBkMode(dc,TRANSPARENT);
            DrawTextW(dc,W("等待学生端画面…\n学生端收到指令后将自动连接并上传屏幕\n(半分辨率 RGB555 · 画面静止时不传输)"),-1,&tr,
                      DT_CENTER|DT_VCENTER|DT_WORDBREAK);
            SelectObject(dc,of);
        }
        BitBlt(wdc,0,0,rc.right,rc.bottom,md,0,0,SRCCOPY);
        SelectObject(md,mdold);
        DeleteObject(mdbm);
        DeleteDC(md);
        EndPaint(w,&ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        rdp_mouse(w,LOWORD(lp),HIWORD(lp),"M");
        return 0;
    case WM_LBUTTONDOWN:
        rdp_mouse(w,LOWORD(lp),HIWORD(lp),"MD|1");
        return 0;
    case WM_LBUTTONUP:
        rdp_mouse(w,LOWORD(lp),HIWORD(lp),"MU|1");
        return 0;
    case WM_RBUTTONDOWN:
        rdp_mouse(w,LOWORD(lp),HIWORD(lp),"MD|2");
        return 0;
    case WM_RBUTTONUP:
        rdp_mouse(w,LOWORD(lp),HIWORD(lp),"MU|2");
        return 0;
    case WM_MOUSEWHEEL:{
        POINT pt;
        char line[96];
        int delta=(int)(short)HIWORD(wp);
        pt.x=(short)LOWORD(lp);
        pt.y=(short)HIWORD(lp);
        ScreenToClient(w,&pt);
        {
            RECT rc,dst;
            UINT nx,ny;
            GetClientRect(w,&rc);
            if(rdp_dst_rect(rc,&dst)){
                if(pt.x<dst.left) pt.x=dst.left;
                if(pt.x>dst.right) pt.x=dst.right;
                if(pt.y<dst.top) pt.y=dst.top;
                if(pt.y>dst.bottom) pt.y=dst.bottom;
                nx=(UINT)((pt.x-dst.left)*65535.0/(dst.right-dst.left));
                ny=(UINT)((pt.y-dst.top)*65535.0/(dst.bottom-dst.top));
                sprintf(line,"W|%u|%u|%d",nx,ny,delta);
                rdp_send(line);
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_KEYUP:
        if(wp==VK_ESCAPE && m==WM_KEYDOWN){
            PostMessage(w,WM_CLOSE,0,0);
            return 0;
        }
        {
            char line[64];
            sprintf(line,"K|%u|%d",(unsigned)wp,(m==WM_KEYDOWN)?1:0);
            rdp_send(line);
        }
        return 0;
    case WM_CLOSE:
        rdp_stop();
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        g_rdpWnd=NULL;
        g_rdpActive=0;
        return 0;
    }
    return DefWindowProcW(w,m,wp,lp);
}

static DWORD WINAPI rdp_recv_thread(LPVOID p){
    SOCKET s=(SOCKET)(INT_PTR)p;
    for(;;){
        unsigned int len;
        unsigned char *pb;
        unsigned *h;
        unsigned w,hh,size;
        int ok=0;
        if(!recv_u32(s,&len)) break;
        if(len<20 || len>16u*1024u*1024u) break;
        pb=(unsigned char*)malloc(len);
        if(!pb) break;
        if(!recv_all(s,(char*)pb,(int)len)){ free(pb); break; }
        h=(unsigned*)pb;
        w=h[2]; hh=h[3]; size=h[4];
        if(h[0]==RDP_MAGIC && w && hh && size==(unsigned)(len-20) && size==w*hh*2){
            EnterCriticalSection(&g_cs);
            if(g_rdpCap<size){
                free(g_rdpFrm);
                g_rdpFrm=(unsigned char*)malloc(size);
                g_rdpCap=g_rdpFrm?size:0;
            }
            if(g_rdpFrm){
                memcpy(g_rdpFrm,pb+20,size);
                g_rdpW=w; g_rdpH=hh;
                ok=1;
            }
            LeaveCriticalSection(&g_cs);
        }
        free(pb);
        if(ok && g_rdpWnd) InvalidateRect(g_rdpWnd,NULL,FALSE);
    }
    closesocket(s);
    EnterCriticalSection(&g_rdpSendCs);
    if(g_rdpSock==s) g_rdpSock=INVALID_SOCKET;
    LeaveCriticalSection(&g_rdpSendCs);
    if(g_rdpWnd) PostMessage(g_rdpWnd,WM_CLOSE,0,0);
    return 0;
}

static DWORD WINAPI rdp_accept_thread(LPVOID arg){
    SOCKET l;
    (void)arg;
    l=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(l==INVALID_SOCKET) return 0;
    {
        struct sockaddr_in a;
        memset(&a,0,sizeof a);
        a.sin_family=AF_INET;
        a.sin_port=htons(RDP_PORT);
        a.sin_addr.s_addr=INADDR_ANY;
        if(bind(l,(struct sockaddr*)&a,sizeof a)!=0){ closesocket(l); return 0; }
        listen(l,4);
    }
    for(;;){
        struct sockaddr_in f;
        int fl=sizeof f;
        SOCKET s=accept(l,(struct sockaddr*)&f,&fl);
        int active;
        if(s==INVALID_SOCKET){ Sleep(200); continue; }
        EnterCriticalSection(&g_rdpSendCs);
        active=g_rdpActive;
        if(active){
            if(g_rdpSock!=INVALID_SOCKET) closesocket(g_rdpSock);
            g_rdpSock=s;
        }
        LeaveCriticalSection(&g_rdpSendCs);
        if(!active){ closesocket(s); continue; }
        CloseHandle(CreateThread(NULL,0,rdp_recv_thread,(LPVOID)(INT_PTR)s,0,NULL));
    }
}

static void open_rdp_window(void){
    static int cls_ok=0;
    RECT r;
    int i;
    SOCKET s;
    char title[300];
    if(!cls_ok){
        WNDCLASSW wc;
        memset(&wc,0,sizeof wc);
        wc.lpfnWndProc=rdp_proc;
        wc.hInstance=g_hInst;
        wc.hIcon=LoadIconW(NULL,IDI_APPLICATION);
        wc.hCursor=LoadCursorW(NULL,IDC_ARROW);
        wc.hbrBackground=NULL;
        wc.lpszClassName=W("CC_T_Rdp");
        RegisterClassW(&wc);
        cls_ok=1;
    }
    if(g_rdpWnd){ SetForegroundWindow(g_rdpWnd); return; }
    i=pick_one_target(g_rdpIp,g_rdpName);
    if(i<0){
        MsgBoxU(g_main,"没有已连接的学生\n(远程桌面一次只针对一台机器, 选中多台时取第一台)","提示",MB_OK|MB_ICONINFORMATION);
        return;
    }
    g_rdpIdx=i;
    EnterCriticalSection(&g_cs);
    g_rdpW=0; g_rdpH=0;
    LeaveCriticalSection(&g_cs);
    r.left=0; r.top=0; r.right=S(1000); r.bottom=S(640);
    AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);
    sprintf(title,"远程桌面 - %s(%s) · 可直接操作鼠标键盘 · Esc 断开",g_rdpName,g_rdpIp);
    g_rdpWnd=CreateWindowExW(0,W("CC_T_Rdp"),W(title),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,
            g_main,NULL,g_hInst,NULL);
    dwm_modern(g_rdpWnd);
    ShowWindow(g_rdpWnd,SW_SHOW);
    UpdateWindow(g_rdpWnd);
    g_rdpActive=1;
    s=ensure_sock(i);
    if(s==INVALID_SOCKET || !send_cmd(s,"RDP_START")){
        MsgBoxU(g_main,"下发远程桌面指令失败(学生端未连接)","错误",MB_OK|MB_ICONERROR);
        rdp_stop();
        DestroyWindow(g_rdpWnd);
        g_rdpWnd=NULL;
    }
}

/* ---------------- 学生列表刷新 ----------------
 * 用数据签名比对: 学生状态无变化时不重建列表/不重绘, 消除 1 秒定时器带来的频闪 */
static char g_sig[65536];
static int  g_sigInit=0;

static void refresh_list(void){
    static char sig[65536];
    int n=0,onl=0,k;
    size_t off=0;
    EnterCriticalSection(&g_cs);
    for(k=0;k<g_nStu && n<MAXSTU;k++){
        STU *s=&g_stu[k];
        if(s->sock!=INVALID_SOCKET) onl++;
        g_map[n]=k;
        off+=(size_t)sprintf(sig+off,"%s;%s;%d;%d;%d|",
                             s->ip,s->name,(s->sock!=INVALID_SOCKET),s->locked,s->demo);
        n++;
    }
    g_rowN=n;
    LeaveCriticalSection(&g_cs);
    sprintf(sig+off,"N%d;O%d;",n,onl);

    if(g_sigInit && strcmp(sig,g_sig)==0) return;   /* 无变化: 跳过全部重绘 */
    strcpy(g_sig,sig);
    g_sigInit=1;
    g_onlN=onl;

    SendMessageW(g_lb,LB_RESETCONTENT,0,0);
    for(k=0;k<n;k++)
        SendMessageW(g_lb,LB_ADDSTRING,0,(LPARAM)"");
    ShowWindow(g_lb,n?SW_SHOW:SW_HIDE);   /* 空列表时显示占位提示 */
    InvalidateRect(g_main,NULL,TRUE);
}

/* ---------------- 绘制控件 ---------------- */
static const char *btn_label(int id){
    switch(id){
    case ID_BTN_MSG:    return "广播消息";
    case ID_BTN_LOCK:   return "锁定屏幕";
    case ID_BTN_UNLOCK: return "解除锁定";
    case ID_BTN_DEMO1:  return "开始广播";
    case ID_BTN_DEMO0:  return "停止广播";
    case ID_BTN_FILE:   return "下发文件";
    case ID_BTN_RDP:    return "远程桌面";
    case ID_BTN_EXEC:   return "远程终端";
    case ID_BTN_SHUT:   return "远程关机";
    case ID_BTN_REBOOT: return "远程重启";
    case ID_BTN_FW:     return "防火墙修复";
    case ID_BTN_ADDIP:  return "指定学生IP";
    }
    return "";
}
static COLORREF btn_dot(int id,int enabled){
    if(!enabled) return RGB(80,86,98);
    switch(id){
    case ID_BTN_MSG:    return C_DOT;
    case ID_BTN_LOCK:   return C_DOTR;
    case ID_BTN_UNLOCK: return C_DOTG;
    case ID_BTN_DEMO1:  return C_DOTP;
    case ID_BTN_DEMO0:  return C_DOTR;
    case ID_BTN_FILE:   return C_DOTN;
    case ID_BTN_RDP:    return C_DOT;
    case ID_BTN_EXEC:   return C_DOTG;
    case ID_BTN_SHUT:   return C_DOTR;
    case ID_BTN_REBOOT: return C_DOTR;
    case ID_BTN_FW:     return C_DOTN;
    case ID_BTN_ADDIP:  return C_DOT;
    }
    return C_DOTN;
}
static void draw_btn(DRAWITEMSTRUCT *d){
    COLORREF bg=C_CARD;
    int en=!(d->itemState & ODS_DISABLED);
    if(!en)                          bg=RGB(36,39,47);
    else if(d->itemState & ODS_SELECTED) bg=RGB(34,38,46);
    else if(d->itemState & ODS_HOTLIGHT) bg=C_CARDH;
    /* 先铺满父背景色, 消除圆角四角露出的系统默认浅色(白边) */
    FillRect(d->hDC,&d->rcItem,g_brBg);
    FillRound(d->hDC,d->rcItem,bg,S(10));
    {
        int cy=(d->rcItem.top+d->rcItem.bottom)/2;
        Dot(d->hDC,d->rcItem.left+S(18),cy,S(5),btn_dot(d->CtlID,en));
        {
            RECT tr=d->rcItem;
            HGDIOBJ of=SelectObject(d->hDC,g_fBtn);
            tr.left=d->rcItem.left+S(34);
            SetBkMode(d->hDC,TRANSPARENT);
            SetTextColor(d->hDC,en?C_TXT:C_TXT3);
            DrawTextW(d->hDC,W(btn_label(d->CtlID)),-1,&tr,
                      DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            SelectObject(d->hDC,of);
        }
    }
}

static int draw_pill(HDC dc,int right,int cy,const wchar_t *text,COLORREF bg,COLORREF fg){
    SIZE sz;
    int pw,ph;
    RECT r;
    HGDIOBJ of=SelectObject(dc,g_fPill);
    GetTextExtentPoint32W(dc,text,(int)wcslen(text),&sz);
    SelectObject(dc,of);
    ph=sz.cy+S(6);
    pw=sz.cx+S(16);
    r.top=cy-ph/2; r.bottom=r.top+ph;
    r.right=right; r.left=r.right-pw;
    FillRound(dc,r,bg,ph/2);
    txtw(dc,r,text,g_fPill,fg,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    return pw+S(6);
}

static void draw_row(DRAWITEMSTRUCT *d){
    STU s;
    RECT card;
    int cy;
    int right;
    wchar_t nm[96],ipp[64];
    if(d->itemID==(UINT)-1 || d->itemID>=(UINT)g_rowN){
        FillRect(d->hDC,&d->rcItem,g_brPanel);
        return;
    }
    EnterCriticalSection(&g_cs);
    {
        int si=g_map[d->itemID];
        if(si>=0 && si<g_nStu) s=g_stu[si];
        else memset(&s,0,sizeof s);
    }
    LeaveCriticalSection(&g_cs);

    card=d->rcItem;
    InflateRect(&card,-S(3),-S(4));
    FillRound(d->hDC,card,(d->itemState & ODS_SELECTED)?C_CARDSL:C_CARD,S(10));
    if(d->itemState & ODS_SELECTED)
        FrameRound(d->hDC,card,C_DOT,S(10),1);
    cy=(card.top+card.bottom)/2;
    Dot(d->hDC,card.left+S(20),cy,S(5),(s.sock!=INVALID_SOCKET)?C_DOTG:RGB(90,96,108));

    MultiByteToWideChar(CP_UTF8,0,s.name[0]?s.name:"(未命名)",-1,nm,96);
    MultiByteToWideChar(CP_UTF8,0,s.ip[0]?s.ip:"?",-1,ipp,64);
    {
        RECT nr=card;
        nr.left=card.left+S(38);
        nr.right=nr.left+S(190);
        nr.top=card.top+S(7);
        nr.bottom=nr.top+S(20);
        txtw(d->hDC,nr,nm,g_fName,C_TXT,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        nr.top=card.top+S(26);
        nr.bottom=nr.top+S(16);
        txtw(d->hDC,nr,ipp,g_fPill,C_TXT2,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
    }
    right=card.right-S(12);
    if(s.demo)
        right-=draw_pill(d->hDC,right,cy,W("广播中"),RGB(34,48,92),RGB(140,180,255));
    if(s.locked)
        right-=draw_pill(d->hDC,right,cy,W("已锁定"),RGB(88,62,14),RGB(250,204,120));
    if(s.sock!=INVALID_SOCKET)
        draw_pill(d->hDC,right,cy,W("在线"),RGB(28,70,48),RGB(126,231,159));
    else
        draw_pill(d->hDC,right,cy,W("未连接"),RGB(40,44,54),RGB(140,146,158));
}

/* ---------------- 布局 ---------------- */
static void layout_children(void){
    static const int ids[10]={ID_BTN_MSG,ID_BTN_LOCK,ID_BTN_UNLOCK,ID_BTN_DEMO1,
                              ID_BTN_DEMO0,ID_BTN_FILE,ID_BTN_RDP,ID_BTN_EXEC,
                              ID_BTN_SHUT,ID_BTN_REBOOT};
    static const int xs[2]={552,730};
    static const int ys[5]={136,186,236,286,336};
    HWND e;
    int k;
    SetWindowPos(g_lb,NULL,S(24),S(96),S(504),S(452),SWP_NOZORDER|SWP_NOACTIVATE);
    SendMessageW(g_lb,LB_SETITEMHEIGHT,0,MAKELPARAM(S(48),0));
    e=GetDlgItem(g_main,ID_EDIT);
    SetWindowPos(e,NULL,S(564),S(96),S(312),S(21),SWP_NOZORDER|SWP_NOACTIVATE);
    for(k=0;k<10;k++){
        HWND b=GetDlgItem(g_main,ids[k]);
        SetWindowPos(b,NULL,S(xs[k%2]),S(ys[k/2]),S(158),S(40),SWP_NOZORDER|SWP_NOACTIVATE);
    }
    {
        HWND fw=GetDlgItem(g_main,ID_BTN_FW);
        if(fw) SetWindowPos(fw,NULL,S(552),S(386),S(166),S(34),SWP_NOZORDER|SWP_NOACTIVATE);
        { HWND ai=GetDlgItem(g_main,ID_BTN_ADDIP); if(ai) SetWindowPos(ai,NULL,S(730),S(386),S(166),S(34),SWP_NOZORDER|SWP_NOACTIVATE); }
    }
    InvalidateRect(g_main,NULL,TRUE);
}

/* ---------------- 主窗口绘制 ---------------- */
static void paint_all(HWND w,HDC dc){
    RECT rc,pan;
    wchar_t sb[200];
    GetClientRect(w,&rc);
    {
        HBRUSH b=CreateSolidBrush(C_BG);
        FillRect(dc,&rc,b);
        DeleteObject(b);
    }
    /* 头部 */
    txt(dc,S(20),S(16),"课堂控制 · 主控端",g_fTitle,C_TXT);
    txt(dc,S(22),S(52),"ClassCtrl v1.5.0 · 教师端 · 广播发现 + 子网反向扫描 双通道",g_fSub,C_TXT2);
    pan.right=rc.right-S(20); pan.left=pan.right-S(170);
    pan.top=S(14); pan.bottom=S(46);
    FillRound(dc,pan,C_PANEL,S(12));
    Dot(dc,pan.left+S(24),pan.top+S(16),S(5),(g_onlN>0)?C_DOTG:RGB(90,96,108));
    wsprintfW(sb,W("在线 %d / 共 %d"),g_onlN,g_rowN);
    {
        RECT tr=pan;
        tr.left=pan.left+S(40);
        txtw(dc,tr,sb,g_fBody,C_TXT,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    }
    /* 分区标签 */
    txt(dc,S(22),S(70),"学生列表",g_fSub,C_TXT2);
    txt(dc,S(554),S(70),"消息与控制",g_fSub,C_TXT2);
    /* 左侧列表面板 */
    pan.left=S(16); pan.top=S(88); pan.right=S(528); pan.bottom=S(556);
    FillRound(dc,pan,C_PANEL,S(14));
    /* 空状态占位 */
    if(g_rowN==0){
        RECT er={pan.left,pan.top+S(120),pan.right,pan.top+S(160)};
        txtw(dc,er,W("等待学生上线…"),g_fBody,C_TXT2,DT_CENTER|DT_SINGLELINE);
        er.top=pan.top+S(155); er.bottom=er.top+S(120);
        txtw(dc,er,W("请在被控端机器上运行 Student.exe\n若已运行仍未上线: 学生机放行防火墙(或点右侧\"防火墙修复\")\n仍不行则学生机命令行运行 Student.exe 教师IP 直连"),
             g_fPill,C_TXT3,DT_CENTER|DT_WORDBREAK);
    }
    /* 消息输入面板 */
    pan.left=S(552); pan.top=S(88); pan.right=S(888); pan.bottom=S(124);
    FillRound(dc,pan,C_CARD,S(10));
    /* 操作提示 */
    {
        RECT hr={S(552),S(432),S(888),S(556)};
        txtw(dc,hr,
             W("Ctrl+点击 多选目标; 广播/锁定/解锁 未选中 = 全体\n远程桌面/终端: 选中一名学生后点击对应按钮\n广播=UDP 组播+各网卡定向广播; 远程桌面=TCP 15668"),
             g_fSub,C_TXT2,DT_WORDBREAK);
    }
    /* 底部状态栏 */
    pan.left=0; pan.top=rc.bottom-S(36); pan.right=rc.right; pan.bottom=rc.bottom;
    {
        HBRUSH b=CreateSolidBrush(C_BAR);
        FillRect(dc,&pan,b);
        DeleteObject(b);
    }
    wsprintfW(sb,W("教师端 IP: %s"),W(g_localIP));
    {
        RECT sr={S(20),rc.bottom-S(26),S(420),rc.bottom-S(8)};
        txtw(dc,sr,sb,g_fPill,C_TXT2,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
    }
    {
        RECT sr={S(430),rc.bottom-S(26),rc.right-S(20),rc.bottom-S(8)};
        txtw(dc,sr,W("UDP 15555/15556 · TCP 15666 · 组播 239.10.10.10 · RDP 15668 · 扫描 15670"),
             g_fPill,C_TXT3,DT_RIGHT|DT_SINGLELINE|DT_END_ELLIPSIS);
    }
}

/* ---------------- 输入框占位符 ---------------- */
static WNDPROC g_editOrig;
static LRESULT CALLBACK edit_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    LRESULT r=CallWindowProcW(g_editOrig,w,m,wp,lp);
    if(m==WM_PAINT && GetWindowTextLengthW(w)==0){
        HDC dc=GetDC(w);
        RECT rc;
        GetClientRect(w,&rc);
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,C_TXT3);
        SelectObject(dc,g_fBody);
        {
            RECT tr=rc;
            tr.left=rc.left+2;
            DrawTextW(dc,W("输入消息内容…"),-1,&tr,
                      DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOCLIP|DT_END_ELLIPSIS);
        }
        ReleaseDC(w,dc);
    }
    return r;
}

static LRESULT CALLBACK main_proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    switch(m){
    case WM_CREATE:{
        HINSTANCE hi=(HINSTANCE)GetWindowLongPtrW(w,GWLP_HINSTANCE);
        g_lb=CreateWindowExW(0,L"LISTBOX",L"",
                WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_TABSTOP|
                LBS_EXTENDEDSEL|LBS_HASSTRINGS|LBS_NOTIFY|
                LBS_OWNERDRAWFIXED|LBS_NOINTEGRALHEIGHT,
                S(24),S(96),S(504),S(452),w,(HMENU)(INT_PTR)ID_LB,hi,NULL);
        {
            HWND e=CreateWindowExW(0,L"EDIT",L"",
                    WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
                    S(564),S(96),S(312),S(21),w,(HMENU)(INT_PTR)ID_EDIT,hi,NULL);
            SendMessageW(e,WM_SETFONT,(WPARAM)g_fBody,MAKELPARAM(1,0));
            g_editOrig=(WNDPROC)SetWindowLongPtrW(e,GWLP_WNDPROC,(LONG_PTR)edit_proc);
        }
        {
            static const int ids[10]={ID_BTN_MSG,ID_BTN_LOCK,ID_BTN_UNLOCK,ID_BTN_DEMO1,
                                      ID_BTN_DEMO0,ID_BTN_FILE,ID_BTN_RDP,ID_BTN_EXEC,
                                      ID_BTN_SHUT,ID_BTN_REBOOT};
            static const int xs[2]={552,730};
            static const int ys[5]={136,186,236,286,336};
            int k;
            for(k=0;k<10;k++){
                HWND b=CreateWindowExW(0,L"BUTTON",L"",
                        WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
                        S(xs[k%2]),S(ys[k/2]),S(158),S(40),
                        w,(HMENU)(INT_PTR)ids[k],hi,NULL);
                (void)b;
            }
        }
        EnableWindow(GetDlgItem(w,ID_BTN_DEMO0),FALSE);
        CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
                S(552),S(386),S(166),S(34),w,(HMENU)(INT_PTR)ID_BTN_FW,hi,NULL);
        CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
                S(730),S(386),S(166),S(34),w,(HMENU)(INT_PTR)ID_BTN_ADDIP,hi,NULL);
        SetTimer(w,1,1000,NULL);
        refresh_list();
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
    case WM_MEASUREITEM:
        ((MEASUREITEMSTRUCT*)lp)->itemHeight=S(48);
        return TRUE;
    case WM_DRAWITEM:{
        DRAWITEMSTRUCT *d=(DRAWITEMSTRUCT*)lp;
        if(d->CtlType==ODT_BUTTON) draw_btn(d);
        else if(d->CtlType==ODT_LISTBOX) draw_row(d);
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:{
        PAINTSTRUCT ps;
        HDC dc=BeginPaint(w,&ps);
        RECT rc;
        HDC md;
        HBITMAP mb;
        HGDIOBJ om;
        GetClientRect(w,&rc);
        md=CreateCompatibleDC(dc);
        mb=CreateCompatibleBitmap(dc,rc.right,rc.bottom);
        om=SelectObject(md,mb);
        paint_all(w,md);
        BitBlt(dc,0,0,rc.right,rc.bottom,md,0,0,SRCCOPY);
        SelectObject(md,om);
        DeleteObject(mb);
        DeleteDC(md);
        EndPaint(w,&ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp,C_TXT);
        SetBkColor((HDC)wp,C_CARD);
        return (LRESULT)(INT_PTR)g_brCard;
    case WM_CTLCOLORBTN:          /* 按钮底层背景用深色, 圆角外无白边 */
        return (LRESULT)(INT_PTR)g_brBg;
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)(INT_PTR)g_brPanel;
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp,C_TXT2);
        SetBkColor((HDC)wp,C_BG);
        return (LRESULT)(INT_PTR)g_brBg;
    case WM_COMMAND:
        switch(LOWORD(wp)){
        case ID_BTN_MSG:    send_msg_all();  break;
        case ID_BTN_LOCK:   do_cmd_all("LOCK");   break;
        case ID_BTN_UNLOCK: do_cmd_all("UNLOCK"); break;
        case ID_BTN_DEMO1: case ID_BTN_DEMO0: demo_toggle(); break;
        case ID_BTN_FILE:   send_file_all(); break;
        case ID_BTN_RDP:    open_rdp_window(); break;
        case ID_BTN_EXEC:   open_exec_dialog(); break;
        case ID_BTN_FW:     fw_repair(); break;
        case ID_BTN_ADDIP:  open_addip_dialog(); break;
        case ID_BTN_SHUT:
            if(MsgBoxU(w,"确认远程关闭所选(未选则为全部)学生计算机吗?","远程关机",
                       MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES)
                do_cmd_all("SHUTDOWN");
            break;
        case ID_BTN_REBOOT:
            if(MsgBoxU(w,"确认远程重启所选(未选则为全部)学生计算机吗?","远程重启",
                       MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES)
                do_cmd_all("REBOOT");
            break;
        }
        return 0;
    case WM_TIMER:
        if(wp==1) refresh_list();
        return 0;
    case WM_APP_REFRESH:
        refresh_list();
        return 0;
    case WM_DESTROY:
        if(g_demoOn) demo_toggle();
        do_cmd_all("UNLOCK");
        KillTimer(w,1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w,m,wp,lp);
}

/* ---------------- 入口 ---------------- */
static void get_local_ip(char *out,int cap){
    SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    out[0]=0;
    if(s!=INVALID_SOCKET){
        struct sockaddr_in a,la;
        int ln=sizeof la;
        memset(&a,0,sizeof a);
        a.sin_family=AF_INET;
        a.sin_port=htons(9);
        a.sin_addr.s_addr=inet_addr("255.255.255.255");
        if(connect(s,(struct sockaddr*)&a,sizeof a)==0 &&
           getsockname(s,(struct sockaddr*)&la,&ln)==0)
            strncpy(out,inet_ntoa(la.sin_addr),cap-1);
        closesocket(s);
    }
    if(!out[0]) strcpy(out,"127.0.0.1");
    out[cap-1]=0;
}

static void dwm_modern(HWND w){
    BOOL dark=TRUE;
    int pref=2;    /* ROUND */
    if(DwmSetWindowAttribute(w,20,&dark,sizeof dark)!=S_OK)
        DwmSetWindowAttribute(w,19,&dark,sizeof dark);
    DwmSetWindowAttribute(w,33,&pref,sizeof pref);
}

/* ---------------- 防火墙自动放行 (与极域安装脚本同思路) ---------------- */
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
        if(RegQueryValueExW(k,L"FwAsked",NULL,&t,(BYTE*)&v,&sz)==ERROR_SUCCESS){
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
        RegSetValueExW(k,L"FwAsked",0,REG_DWORD,(const BYTE*)&v,4);
        RegCloseKey(k);
    }
}
/* 生成 netsh 批处理 (bat 写 ANSI/GBK, 保证含中文路径也能被 cmd 解析) */
static void fw_write_bat_t(const wchar_t *bat){
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
        "netsh advfirewall firewall add rule name=\"ClassCtrl Teacher\" dir=in action=allow program=\"%s\" enable=yes >nul 2>&1\r\n"
        "netsh advfirewall firewall add rule name=\"ClassCtrl Ports\" dir=in action=allow protocol=TCP localport=15666-15668 enable=yes >nul 2>&1\r\n"
        "netsh advfirewall firewall add rule name=\"ClassCtrl Udp\" dir=in action=allow protocol=UDP localport=15555-15556 enable=yes >nul 2>&1\r\n",
        apath,apath);
    h=CreateFileW(bat,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    if(h!=INVALID_HANDLE_VALUE){
        WriteFile(h,buf,(DWORD)strlen(buf),&wr,NULL);
        CloseHandle(h);
    }
    free(buf);
}
static void fw_run_bat(const wchar_t *bat,int elevated){
    if(elevated)
        ShellExecuteW(NULL,L"runas",bat,NULL,NULL,SW_HIDE);
    else
        ShellExecuteW(NULL,L"open",bat,NULL,NULL,SW_HIDE);
}
static void fw_setup_teacher(void){
    wchar_t temp[400],bat[560];
    if(GetTempPathW(400,temp)==0) return;
    lstrcpyW(bat,temp);
    lstrcatW(bat,L"ClassCtrl_fw_teacher.bat");
    if(fw_is_admin()){
        fw_write_bat_t(bat);
        fw_run_bat(bat,0);
        return;
    }
    if(fw_reg_asked()) return;
    if(MsgBoxU(g_main,
        "局域网使用需要放行 Windows 防火墙, 否则学生端将无法连接。\n\n"
        "现在自动添加防火墙放行规则吗?\n(将弹出一次管理员授权窗口, 之后不再询问)",
        "防火墙设置",MB_YESNO|MB_ICONQUESTION)==IDYES){
        fw_write_bat_t(bat);
        fw_run_bat(bat,1);
    }
    fw_reg_set_asked();
}
/* 手动触发的防火墙修复 (总是尝试提权) */
static void fw_repair(void){
    wchar_t temp[400],bat[560];
    if(GetTempPathW(400,temp)==0) return;
    lstrcpyW(bat,temp);
    lstrcatW(bat,L"ClassCtrl_fw_teacher.bat");
    fw_write_bat_t(bat);
    fw_run_bat(bat,1);
    MsgBoxU(g_main,
        "已提交防火墙修复, 请在弹出的授权窗口中点击\"是\"。\n\n"
        "若学生端仍未上线:\n1. 确认学生机也已运行 Student.exe 并放行其防火墙;\n"
        "2. 学生机命令行运行 Student.exe 教师IP (IP 见底部状态栏) 直连;\n"
        "3. 两机互 ping 检查网络是否可达。",
        "防火墙修复",MB_OK|MB_ICONINFORMATION);
}

int WINAPI WinMain(HINSTANCE hi,HINSTANCE hp,LPSTR cl,int show){
    WNDCLASSW wc;
    MSG m;
    WSADATA wd;
    RECT r;
    (void)hp;
    if(WSAStartup(MAKEWORD(2,2),&wd)!=0) return 1;
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_rdpSendCs);
    g_hInst=hi;
    enable_dpi_aware();
    g_dpi=(UINT)GetDeviceCaps(GetDC(NULL),LOGPIXELSX);
    get_local_ip(g_localIP,sizeof g_localIP);
    /* 命令行额外扫描网段: Teacher.exe 192.168.32.0 192.168.33.0 ... (跨网段部署) */
    if(cl && cl[0]){
        char *tok=strtok(cl," \t");
        while(tok && g_nExtra<16){
            unsigned long a=inet_addr(tok);
            if(a!=INADDR_NONE){
                unsigned ip=ntohl(a);
                sprintf(g_extraSub[g_nExtra],"%u.%u.%u.0",
                        (unsigned)((ip>>24)&255),(unsigned)((ip>>16)&255),(unsigned)((ip>>8)&255));
                g_nExtra++;
            }
            tok=strtok(NULL," \t");
        }
    }

    memset(&wc,0,sizeof wc);
    wc.lpfnWndProc=main_proc;
    wc.hInstance=hi;
    wc.hIcon=LoadIconW(NULL,IDI_APPLICATION);
    wc.hCursor=LoadCursorW(NULL,IDC_ARROW);
    wc.hbrBackground=NULL;
    wc.lpszClassName=W("CC_Teacher_Main");
    RegisterClassW(&wc);
    wc.lpfnWndProc=exec_proc;
    wc.lpszClassName=W("CC_T_Exec");
    RegisterClassW(&wc);
    wc.lpfnWndProc=rdp_proc;
    wc.lpszClassName=W("CC_T_Rdp");
    RegisterClassW(&wc);

    make_fonts();
    g_brBg=CreateSolidBrush(C_BG);
    g_brPanel=CreateSolidBrush(C_PANEL);
    g_brCard=CreateSolidBrush(C_CARD);

    r.left=0; r.top=0; r.right=S(904); r.bottom=S(600);
    AdjustWindowRect(&r,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);

    g_main=CreateWindowExW(0,W("CC_Teacher_Main"),W(APP_TITLE),
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,
            CW_USEDEFAULT,CW_USEDEFAULT,
            r.right-r.left,r.bottom-r.top,
            NULL,NULL,hi,NULL);
    dwm_modern(g_main);
    ShowWindow(g_main,show);
    UpdateWindow(g_main);
    fw_setup_teacher();

    CloseHandle(CreateThread(NULL,0,beacon_thread,NULL,0,NULL));
    CloseHandle(CreateThread(NULL,0,subnet_beacon_thread,NULL,0,NULL));
    CloseHandle(CreateThread(NULL,0,udp_thread,NULL,0,NULL));
    CloseHandle(CreateThread(NULL,0,scan_thread,NULL,0,NULL));
    CloseHandle(CreateThread(NULL,0,accept_thread,NULL,0,NULL));
    CloseHandle(CreateThread(NULL,0,rdp_accept_thread,NULL,0,NULL));

    while(GetMessageW(&m,NULL,0,0)>0){
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    WSACleanup();
    DeleteCriticalSection(&g_cs);
    return 0;
}
