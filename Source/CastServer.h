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
 *  GET /        → 投屏网页（深色歌词页，逐字高亮，内置轮询脚本）
 *  GET /state   → {"t":<播放秒数>,"v":<歌词版本>}（页面 120ms 轮询一次）
 *  GET /lyrics  → {"v":<版本>,"lines":[{"t":<开始秒>,"text":".."},..]}
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
        else if (path.startsWith ("/hex"))
        {
            // 诊断：比较「原始字面量字节」与「经 juce::String 转换后的字节」
            const char* raw = pageHtml();
            const char* titleRaw = std::strstr (raw, "<title>");
            juce::String viaString (titleRaw != nullptr ? titleRaw : raw);

            body = "raw=" + juce::String::toHexString ((const void*) (titleRaw != nullptr ? titleRaw : raw), 32)
                 + " viaString=" + juce::String::toHexString ((const void*) viaString.toRawUTF8(), 32);
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
            body = pageHtml();
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
font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;
display:flex;flex-direction:column;align-items:center;justify-content:center;overflow:hidden}
#brand{position:fixed;top:16px;left:20px;font-size:14px;color:#8a8a95;letter-spacing:.2px}
#prev,#next{font-size:24px;color:#6e6e78;margin:18px 0;max-width:90vw;text-align:center;
white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#status{position:fixed;top:16px;right:20px;font-size:12px;color:#8a8a95}
#cur{font-size:7vw;font-weight:600;color:#9a9aa5;margin:24px 0;max-width:92vw;
text-align:center;line-height:1.5}
#cur span.on{color:#f2f2f4}
</style></head><body><div id="brand">&#9679; LyricSync</div>
<div id="status">连接中…</div>
<div id="prev"></div><div id="cur"></div><div id="next"></div>
<script>
let v=-1,lines=[],pi=-2,ok=false;
const st=document.getElementById('status');
async function tick(){
 try{
  const s=await (await fetch('/state')).json();
  if(!ok){ok=true;st.textContent='已连接宿主';}
  if(s.v!==v){v=s.v;const d=await (await fetch('/lyrics')).json();lines=d.lines||[];
    st.textContent='已连接宿主 · '+lines.length+' 句歌词';}
  render(s.t);
 }catch(e){ok=false;st.textContent='连接断开，正在重试…';}
 setTimeout(tick,120);
}
function render(t){
 const el=document.getElementById('cur'),pv=document.getElementById('prev'),
       nx=document.getElementById('next');
 let cur=-1;
 for(let i=0;i<lines.length;i++) if(lines[i].t<=t+0.02) cur=i;
 if(cur<0){
  if(lines.length){
   // 还没开播：直接显示第一句（灰字），播放到它时逐字点亮
   el.innerHTML='';
   for(const ch of lines[0].text){
    const sp=document.createElement('span');sp.textContent=ch;el.appendChild(sp);
   }
   pv.textContent='';
   nx.textContent=lines.length>1?lines[1].text:'';
   st.textContent='等待播放开始 · 共 '+lines.length+' 句';
  }else{
   el.textContent='等待歌词…';
  }
  return;
 }
 if(cur!==pi){
  pi=cur;
  el.innerHTML='';
  for(const ch of lines[cur].text){
   const sp=document.createElement('span');sp.textContent=ch;el.appendChild(sp);
  }
  pv.textContent=cur>0?lines[cur-1].text:'';
  nx.textContent=cur+1<lines.length?lines[cur+1].text:'';
 }
 const end=cur+1<lines.length?lines[cur+1].t:lines[cur].t+4;
 let p=(t-lines[cur].t)/Math.max(0.001,end-lines[cur].t);
 p=Math.max(0,Math.min(1,p));
 const spans=el.children,n=spans.length,k=Math.floor(p*n);
 for(let i=0;i<n;i++)spans[i].className=(i<k||p>=1)?'on':'';
}
tick();
</script></body></html>)HTML";
    }

    std::unique_ptr<juce::StreamingSocket> listener;
    std::atomic<bool> quitFlag { false };
    std::atomic<int>  acceptedCount { 0 };
    int port = 8089;
};
