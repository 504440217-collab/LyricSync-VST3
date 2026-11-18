#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>

#if JUCE_WINDOWS
 #include <winsock2.h>
#endif

//==============================================================================
/**
 *  极简 HTTP 服务：为「局域网网页投屏」提供三个接口。
 *
 *  GET /        → 投屏网页（多行滚动歌词：当前句居中、上下各数行按距离缩放淡出，
 *                  颜色/逐字逐句与舞台一致，rAF 本地插值）
 *  GET /state   → {"t":<播放秒数>,"v":<版本>,"w":<写入时钟>,"p":<播放0/1>}
 *  GET /lyrics  → {"v":<版本>,"fg":<未唱基色>,"mode":<0逐句/1逐字>,
 *                  "lines":[{"t":"e":"c":"u":"o":[逐字偏移],"text":".."},..]}
 *
 *  数据由编辑器（GUI 线程）在 dataLock 保护下写入 stateJson / lyricsJson，
 *  服务线程只读拷贝，互不越界。每个请求一个短连接，浏览器端自行重连。
 */
class CastServer : private juce::Thread
{
public:
    CastServer() : juce::Thread ("LyricSyncCast"), castPool (6) {}

    ~CastServer() override { stop(); }

    /** 在 preferredPort 起的几个端口上尝试监听，成功返回 true。 */
    bool start (int preferredPort)
    {
        stop();

        int port = preferredPort;

        for (int attempt = 0; attempt < 6; ++attempt)
        {
            auto sock = std::make_unique<juce::StreamingSocket>();

            if (sock->createListener (port, "0.0.0.0"))
            {
                listener = std::move (sock);
                this->port = port;
                quitFlag = false;
                startThread();
                return true;
            }

            ++port;
        }

        return false;
    }

    void stop()
    {
        if (isThreadRunning())
        {
            quitFlag = true;

            // 线程可能正阻塞在 accept 上：自己连一次把它打醒
            {
                juce::StreamingSocket wake;
                wake.connect ("127.0.0.1", port, 300);
                wake.close();
            }

            stopThread (2000);
        }

        castPool.removeAllJobs (true, 1000);

        if (listener != nullptr)
        {
            listener->close();
            listener.reset();
        }
    }

    int  getPort() const noexcept    { return port; }
    bool isRunning() const noexcept  { return listener != nullptr && isThreadRunning(); }

    // 编辑器（GUI 线程）写、服务线程读，全部走这把锁
    juce::CriticalSection dataLock;
    juce::ThreadPool   castPool;      // 每个客户端连接一个短任务，互不阻塞
    juce::String lyricsJson = "{\"v\":0,\"lines\":[]}";
    juce::String stateJson  = "{\"t\":0.0,\"v\":0}";

private:
    void run() override
    {
        // 直接阻塞 accept：waitUntilReady 对监听 socket 在部分系统上不可靠，
        // 会导致连接一直躺在内核队列里没人处理（浏览器永远白屏）。
        while (! quitFlag && ! threadShouldExit())
        {
            if (listener == nullptr)
                break;

            auto* client = listener->waitForNextConnection();

            if (client == nullptr)
            {
                juce::Thread::sleep (15);
                continue;
            }

            ++acceptedCount;

            // 每个连接丢进线程池，慢客户端不阻塞其它设备
            castPool.addJob ([this, client]()
            {
                handleClient (*client);
                client->close();
                delete client;
            });
        }
    }

    void handleClient (juce::StreamingSocket& client)
    {
        // 给连接设「接收超时」（SO_RCVTIMEO）：阻塞 read 有数据立刻返回、
        // 1.5 秒没数据返回 -1。绕开 waitUntilReady 的不可靠（它会把真请求也当空）。
       #if JUCE_WINDOWS
        {
            const auto raw = (SOCKET) (juce::pointer_sized_int) client.getRawSocketHandle();

            if (raw != INVALID_SOCKET)
            {
                // accept 来的 socket 可能是非阻塞的：write 会立刻返回 0/-1，
                // 响应永远发不出去（浏览器白屏）。强制改回阻塞模式。
                u_long mode = 0;
                ioctlsocket (raw, FIONBIO, &mode);

                DWORD recvMs = 1500;
                DWORD sendMs = 4000;
                setsockopt (raw, SOL_SOCKET, SO_RCVTIMEO,
                            reinterpret_cast<const char*> (&recvMs), sizeof (recvMs));
                setsockopt (raw, SOL_SOCKET, SO_SNDTIMEO,
                            reinterpret_cast<const char*> (&sendMs), sizeof (sendMs));
            }
        }
       #endif

        juce::String request;

       #if JUCE_WINDOWS
        // 关键：JUCE 的 read() 每次都会把 socket 强制改回非阻塞（源码行为），
        // 阻塞模式/接收超时全被覆盖 —— 所以这里直接用原生 recv。
        // socket 已在上面设为阻塞 + SO_RCVTIMEO(1.5s)：有数据立刻返回，超时返回 -1。
        {
            const auto raw = (SOCKET) (juce::pointer_sized_int) client.getRawSocketHandle();

            if (raw != INVALID_SOCKET)
            {
                for (int guard = 0; guard < 4; ++guard)
                {
                    char buf[1024] = {};
                    const int n = ::recv (raw, buf, sizeof (buf) - 1, 0);

                    if (n > 0)
                    {
                        request += juce::String::fromUTF8 (buf, (size_t) n);

                        if (request.contains ("\r\n\r\n"))
                            break;

                        continue;
                    }

                    break;      // 0 = 对端关闭；-1 = 超时（预连接）或错误
                }
            }
        }
       #else
        const double rDeadline = juce::Time::getMillisecondCounterHiRes() + 1200.0;

        while (juce::Time::getMillisecondCounterHiRes() < rDeadline)
        {
            char buf[1024] = {};
            const int n = client.read (buf, sizeof (buf) - 1, false);

            if (n > 0)
            {
                request += juce::String::fromUTF8 (buf, (size_t) n);

                if (request.contains ("\r\n\r\n"))
                    break;
            }
            else
            {
                juce::Thread::sleep (8);
            }
        }
       #endif

        auto logLine = [] (const juce::String& text)
        {
            juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("LyricSync-cast.log")
                .appendText (juce::Time::getCurrentTime().toString (false, true, true, true)
                             + "  " + text + "\r\n", false, false, "\r\n");
        };

        logLine ("conn accepted=" + juce::String (acceptedCount.load())
                 + " request='" + request.substring (0, 60).replace ("\r", "").replace ("\n", " ")
                 + "' bytes=" + juce::String (request.length()));

        if (request.isEmpty())
        {
            logLine ("empty request -> closed");
            return;                            // 预连接 / 空连接：丢弃
        }

        const juce::String head (request);

        // "GET /path HTTP/1.1"
        const int sp1 = head.indexOfChar (' ');
        // 注意 JUCE 的两参重载是 (startIndex, characterToFind) —— 顺序不能写反！
        const int sp2 = head.indexOfChar (sp1 + 1, ' ');

        if (sp1 < 0 || sp2 <= sp1)
        {
            juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("LyricSync-cast.log")
                .appendText ("bad request line: sp1=" + juce::String (sp1)
                             + " sp2=" + juce::String (sp2) + "\r\n", false, false, "\r\n");
            return;
        }

        const juce::String path = head.substring (sp1 + 1, sp2);

        juce::String body;
        juce::String mime = "application/json; charset=utf-8";

        if (path.startsWith ("/state"))
        {
            const juce::ScopedLock sl (dataLock);
            body = stateJson;
        }
        else if (path.startsWith ("/lyrics"))
        {
            const juce::ScopedLock sl (dataLock);
            body = lyricsJson;
        }
        else if (path.startsWith ("/ping"))
        {
            body = "pong";
            mime = "text/plain; charset=utf-8";
        }
        else if (path.startsWith ("/diag"))
        {
            const int lyricBytes = [this]
            {
                const juce::ScopedLock sl (dataLock);
                return lyricsJson.getNumBytesAsUTF8();
            }();

            body = "accepted=" + juce::String (acceptedCount.load())
                 + " port=" + juce::String (port)
                 + " lyricBytes=" + juce::String (lyricBytes);
            mime = "text/plain; charset=utf-8";
        }
        else
        {
            // 关键：juce::String 的 const char* 构造按 ASCII/Latin-1 处理，
            // 页面里的中文必须显式声明 UTF-8，否则每个字节被当 Latin-1 → 乱码
            body = juce::String (juce::CharPointer_UTF8 (pageHtml()));
            mime = "text/html; charset=utf-8";
        }

        const auto bytes = juce::String (
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: " + mime + "\r\n"
            "Content-Length: " + juce::String (body.getNumBytesAsUTF8()) + "\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n") + body;

        // socket 的 write 可能只发出一部分（接收窗口小/缓冲满），
        // 必须循环发送到全部写完，否则浏览器拿到半截页面（黑屏无字）。
        // 同时加 4 秒总时限，防止任何异常把线程卡死。
        const auto utf8 = bytes.toUTF8();
        const char* ptr = utf8.getAddress();
        int remaining = (int) utf8.sizeInBytes() - 1;
        int totalSent = 0;

        const double wDeadline = juce::Time::getMillisecondCounterHiRes() + 4000.0;

       #if JUCE_WINDOWS
        const auto rawOut = (SOCKET) (juce::pointer_sized_int) client.getRawSocketHandle();

        while (remaining > 0 && rawOut != INVALID_SOCKET
               && juce::Time::getMillisecondCounterHiRes() < wDeadline)
        {
            const int sent = ::send (rawOut, ptr, remaining, 0);

            if (sent <= 0)
            {
                juce::Thread::sleep (5);
                continue;
            }

            ptr += sent;
            remaining -= sent;
            totalSent += sent;
        }
       #else
        while (remaining > 0 && juce::Time::getMillisecondCounterHiRes() < wDeadline)
        {
            const int sent = client.write (ptr, remaining);

            if (sent <= 0)
            {
                juce::Thread::sleep (5);
                continue;
            }

            ptr += sent;
            remaining -= sent;
            totalSent += sent;
        }
       #endif

        {
            juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("LyricSync-cast.log")
                .appendText (juce::Time::getCurrentTime().toString (false, true, true, true)
                             + "  resp path=" + path + " total=" + juce::String ((int) utf8.sizeInBytes() - 1)
                             + " sent=" + juce::String (totalSent)
                             + (remaining > 0 ? " INCOMPLETE" : " ok")
                             + "\r\n", false, false, "\r\n");
        }
    }

    static const char* pageHtml()
    {
        return R"HTML(<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LyricSync 投屏</title><style>
body{margin:0;height:100vh;background:#08080a;color:#a1a1aa;
font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;overflow:hidden}
#brand{position:fixed;top:16px;left:20px;font-size:14px;color:#8a8a95;letter-spacing:.2px;z-index:2}
#status{position:fixed;top:16px;right:20px;font-size:12px;color:#8a8a95;z-index:2}
#lines{position:absolute;top:0;left:0;width:100%;height:100%}
/* 只有容器整体平移，行自身位置固定 —— 换句不会把各行挤成一团（消除跳闪白条） */
#track{position:absolute;left:0;top:0;width:100%;will-change:transform;
transition:transform .30s cubic-bezier(.25,.8,.25,1)}
.ln{position:absolute;left:0;width:100%;display:flex;align-items:center;justify-content:center;
white-space:nowrap;font-weight:600;overflow:hidden;
transition:font-size .22s ease,opacity .22s linear}
.ln span{transition:color .08s linear}
#empty{position:absolute;top:50%;left:0;width:100%;transform:translateY(-50%);
text-align:center;font-size:20px;color:#8a8a95;display:none}
</style></head><body>
<div id="brand">&#9679; LyricSync</div>
<div id="status">连接中…</div>
<div id="lines"><div id="track"></div></div><div id="empty">等待歌词…</div>
<script>
const VIEW=3;                            // 当前句上下各显示几行（共 2*VIEW+1 行）
let v=-1,lines=[],rows=[],ok=false,lastCur=-999,needLayout=true;
let mode=1,fg='#f2f2f4';                 // 逐字/逐句模式 + 未唱基色（来自舞台）
let anchorT=0,anchorWall=0,playing=0;    // 时间锚点：服务器 t 对应的本地单调时刻
let fontBase=40,rowH=56,nearFont=24;
const st=document.getElementById('status');
const track=document.getElementById('track'),empty=document.getElementById('empty');

function metrics(){
  const W=window.innerWidth,H=window.innerHeight;
  // 当前句明显大：其它句缩到约 55%，一眼区分、不会看错行
  fontBase=Math.max(26,Math.min(64,Math.min(W*0.085,H*0.085)));
  nearFont=fontBase*0.55;
  // 行距按「当前句大字号」留足，避免上下行贴脸；再收紧到屏幕能放下 7 行
  rowH=Math.max(fontBase*1.15,Math.min(H/7.4,fontBase*1.7));
}
window.addEventListener('resize',function(){
  metrics();
  // 尺寸变了要重排每行的固定 top/height（否则滚动位置会错）
  for(let i=0;i<rows.length;i++){rows[i].el.style.top=(i*rowH)+'px';
    rows[i].el.style.height=rowH+'px';}
  needLayout=true;
});
metrics();

// 用锚点 + 本地经过时间推算「此刻」的播放秒数：轮询间隔内平滑推进，不滞后
function nowT(){
  return playing ? anchorT+(performance.now()-anchorWall)/1000 : anchorT;
}
async function tick(){
 try{
  const r=await fetch('/state');const s=await r.json();
  if(!ok){ok=true;st.textContent='已连接宿主';}
  if(s.v!==v){
    v=s.v;
    const d=await (await fetch('/lyrics')).json();
    lines=d.lines||[]; mode=(typeof d.mode==='number')?d.mode:1; fg=d.fg||'#f2f2f4';
    st.textContent='已连接宿主 · '+lines.length+' 句歌词';
    buildRows(); needLayout=true; lastCur=-999;
  }
  anchorT=s.t; anchorWall=performance.now(); playing=s.p?1:0;
 }catch(e){ok=false;st.textContent='连接断开，正在重试…';}
 setTimeout(tick,60);
}

function buildRows(){
  track.textContent=''; rows=[];
  empty.style.display=lines.length?'none':'block';
  for(let i=0;i<lines.length;i++){
    const el=document.createElement('div'); el.className='ln';
    // 每行的纵向位置固定：top = 行号 * rowH，永不再改（这是消除跳闪的关键）
    el.style.top=(i*rowH)+'px';
    el.style.height=rowH+'px';
    const sps=[];
    for(const ch of lines[i].text){const sp=document.createElement('span');
      sp.textContent=ch;sp.style.color=fg;el.appendChild(sp);sps.push(sp);}
    track.appendChild(el); rows.push({el:el,spans:sps});
  }
}

// 复刻舞台 sungProgressAt：返回「已唱到第几个字」的连续浮点；无逐字时间戳返回 -1
function sungProgress(l,t){
 const o=l.o; if(!o||!o.length||!l.text.length) return -1;
 const rel=t-l.t; let count=0;
 for(const off of o){ if(rel>=off) ++count; else break; }
 if(count<=0) return rel<0?-1:0;
 const idx=count-1, start=o[idx];
 const end=(idx+1<o.length)?o[idx+1]:start+0.4;
 const span=Math.max(0.02,end-start);
 return idx+Math.max(0,Math.min(1,(rel-start)/span));
}
function mix(a,b,t){
 const pa=hex(a),pb=hex(b);
 return 'rgb('+Math.round(pa[0]+(pb[0]-pa[0])*t)+','+
                Math.round(pa[1]+(pb[1]-pa[1])*t)+','+
                Math.round(pa[2]+(pb[2]-pa[2])*t)+')';
}
function hex(s){const v=parseInt(s.slice(1),16);return [(v>>16)&255,(v>>8)&255,v&255];}

// 换句只平移容器（整体滚动，绝不重叠）；每帧仅更新当前句演唱进度色
function paint(t){
 if(!lines.length) return;
 let cur=-1;
 for(let i=0;i<lines.length;i++) if(lines[i].t<=t+0.02) cur=i;
 if(cur<0) cur=0;                          // 未开播：把第一句当中心，但按进度不着色
 if(needLayout||cur!==lastCur){
   lastCur=cur; needLayout=false;
   // 容器平移：让第 cur 行的中心落到视口正中（唯一带 transition 的属性）
   const H=window.innerHeight;
   track.style.transform='translateY('+(H/2 - (cur*rowH+rowH/2)).toFixed(1)+'px)';
   for(let i=0;i<rows.length;i++){
     const d=i-cur,ad=Math.abs(d),row=rows[i],el=row.el;
     if(ad>VIEW){el.style.opacity='0';el.style.visibility='hidden';continue;}
     el.style.visibility='visible';
     // 当前句满字号，其余统一缩小到 nearFont 并按距离再淡出，主次一眼分明
     el.style.fontSize=(ad===0?fontBase:nearFont*Math.max(0.82,1-0.12*(ad-1))).toFixed(1)+'px';
     el.style.opacity=String(ad===0?1:Math.max(0.14,0.55-0.16*(ad-1)));
     if(ad!==0){const u=(lines[i].u)||fg;
       for(const sp of row.spans) sp.style.color=u;}   // 非当前句：该行未唱色
   }
 }
 const row=rows[cur]; if(!row) return;
 const L=lines[cur],tint=L.c||fg,unsung=L.u||fg,spans=row.spans,n=spans.length;
 if(mode===0){
   const p=Math.max(0,Math.min(1,(t-L.t)/0.22));
   for(let i=0;i<n;i++)spans[i].style.color=p>=1?tint:mix(unsung,tint,p);
 }else{
   let sung=sungProgress(L,t);
   if(sung<0) sung=Math.max(0,Math.min(1,(t-L.t)/Math.max(0.05,L.e-L.t)))*n;
   for(let i=0;i<n;i++)spans[i].style.color=(i<sung)?tint:unsung;
 }
}
function frame(){ paint(nowT()); requestAnimationFrame(frame); }
tick(); requestAnimationFrame(frame);
</script></body></html>)HTML";
    }

    std::unique_ptr<juce::StreamingSocket> listener;
    std::atomic<bool> quitFlag { false };
    std::atomic<int>  acceptedCount { 0 };
    int port = 8089;
};
