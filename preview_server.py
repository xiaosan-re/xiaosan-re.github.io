#!/usr/bin/env python3
# 博客文章实时预览：根目录=博客目录（/images/... 直接生效），每次请求实时渲染 .md，
# 页面轮询 mtime，存盘即自动刷新并保留滚动位置。
#   用法： python3 preview_server.py [_posts/xxx.md]   默认本篇
import http.server, socketserver, os, sys, re, urllib.parse
import markdown

BLOG = os.path.dirname(os.path.abspath(__file__))
os.chdir(BLOG)
MD = sys.argv[1] if len(sys.argv) > 1 else "_posts/2026-06-11-ai-app-protocol.md"
PORT = 8777

CSS = """
body{background:#fff;color:#24292f;font-family:-apple-system,'PingFang SC','Microsoft YaHei',Segoe UI,Roboto,Helvetica,Arial,sans-serif;line-height:1.75;max-width:840px;margin:0 auto;padding:28px 20px 80px}
h1{font-size:26px;border-bottom:2px solid #e5e7eb;padding-bottom:10px}
h2{font-size:21px;margin-top:36px;border-bottom:1px solid #eaecef;padding-bottom:6px}
h3{font-size:17px;margin-top:24px}
p,li{font-size:15px}
img{max-width:100%;border:1px solid #eef2f7;border-radius:8px;display:block;margin:14px 0}
table{border-collapse:collapse;width:100%;margin:14px 0;font-size:14px}
th,td{border:1px solid #d0d7de;padding:7px 11px;text-align:left;vertical-align:top}
th{background:#f6f8fa;font-weight:700}
code{background:#f3f4f6;padding:1px 5px;border-radius:4px;font-family:ui-monospace,Menlo,monospace;font-size:13px}
pre{background:#f6f8fa;border:1px solid #e5e7eb;border-radius:8px;padding:13px 15px;overflow:auto}
pre code{background:none;padding:0;font-size:12.5px;line-height:1.55}
blockquote{border-left:4px solid #d0d7de;margin:14px 0;padding:4px 16px;color:#57606a;background:#f6f8fa;border-radius:0 6px 6px 0}
a{color:#0969da;text-decoration:none}
#live{position:fixed;top:8px;right:10px;font-size:11px;color:#16a34a;background:#dcfce7;border:1px solid #86efac;border-radius:5px;padding:2px 8px}
"""

JS = """<div id="live">● live</div><script>
let last=null;
async function poll(){try{const m=await (await fetch('/_mtime?'+Date.now())).text();
  if(last&&m!==last){sessionStorage.sy=window.scrollY;location.reload();} last=m;}catch(e){}}
setInterval(poll,1200);
addEventListener('load',()=>{const y=sessionStorage.sy;if(y){scrollTo(0,+y);sessionStorage.removeItem('sy');}});
</script>"""

def img_mtime(src):
    # /images/... → 本地文件 mtime（防缓存戳 + 触发自动刷新）
    rel = src.lstrip('/')
    fp = os.path.join(BLOG, rel)
    try: return os.path.getmtime(fp)
    except OSError: return 0

def referenced_imgs(html):
    return re.findall(r'<img[^>]+src="(/images/[^"?]+)"', html)

def bust(html):
    # 给每个 /images/... 加 ?v=<mtime> 防缓存
    def rep(m):
        src = m.group(1)
        return m.group(0).replace(src, f"{src}?v={int(img_mtime(src))}")
    return re.sub(r'<img[^>]+src="(/images/[^"?]+)"', rep, html)

def combined_mtime():
    # .md + 所有引用图片 的最大 mtime —— 改图也会触发自动刷新
    s = open(MD, encoding="utf-8").read()
    html = markdown.markdown(s, extensions=['tables', 'fenced_code', 'sane_lists', 'attr_list'])
    m = os.path.getmtime(MD)
    for src in referenced_imgs(html):
        m = max(m, img_mtime(src))
    return m

def render():
    s = open(MD, encoding="utf-8").read()
    title = (re.search(r'^title:\s*(.+)$', s, re.M) or [None, "预览"])[1].strip()
    if s.startswith("---"):
        s = s.split("---", 2)[2]
    body = markdown.markdown(s, extensions=['tables', 'fenced_code', 'sane_lists', 'attr_list'])
    body = bust(body)
    return ("<!doctype html><html lang=zh><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            f"<title>{title}</title><style>{CSS}</style></head><body>{JS}"
            f"<h1>{title}</h1>{body}</body></html>")

class H(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        p = urllib.parse.urlparse(self.path).path
        if p in ('/', '/preview'):
            data = render().encode("utf-8"); ct = "text/html; charset=utf-8"
        elif p == '/_mtime':
            data = str(combined_mtime()).encode(); ct = "text/plain"
        else:
            # 图片等静态资源：禁缓存，保证改了 SVG 立刻拿到新版
            self.send_header_nocache = True
            return super().do_GET()
        self.send_response(200); self.send_header("Content-Type", ct)
        self.send_header("Content-Length", str(len(data))); self.end_headers()
        self.wfile.write(data)
    def end_headers(self):
        if getattr(self, "send_header_nocache", False):
            self.send_header("Cache-Control", "no-store, must-revalidate")
            self.send_header("Pragma", "no-cache")
        super().end_headers()
    def log_message(self, *a): pass

socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(("127.0.0.1", PORT), H) as srv:
    print(f"实时预览 → http://127.0.0.1:{PORT}/   (文章: {MD})")
    print("改 .md 存盘即自动刷新，滚动位置保留。Ctrl+C 停止。")
    srv.serve_forever()
