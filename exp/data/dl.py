import urllib.request, os, sys, threading, time

URL = sys.argv[1]
OUT = sys.argv[2]
N = int(sys.argv[3]) if len(sys.argv) > 3 else 8

req = urllib.request.Request(URL, method='HEAD')
try:
    size = int(urllib.request.urlopen(req, timeout=30).headers['Content-Length'])
except Exception:
    req = urllib.request.Request(URL, method='GET')
    size = int(urllib.request.urlopen(req, timeout=30).headers.get('Content-Length', 0))
print(f'total size: {size}', flush=True)

if os.path.exists(OUT) and os.path.getsize(OUT) == size:
    print('already complete, skip'); sys.exit(0)

chunk = size // N
done = [False] * N

def dl(i):
    s = i * chunk
    e = s + chunk - 1 if i < N - 1 else size - 1
    for attempt in range(5):
        try:
            req = urllib.request.Request(URL, headers={'Range': f'bytes={s}-{e}', 'User-Agent': 'Mozilla/5.0'})
            data = urllib.request.urlopen(req, timeout=120).read()
            with open(OUT, 'r+b') as f:
                f.seek(s); f.write(data)
            done[i] = True
            print(f'part {i} done ({len(data)} bytes)', flush=True)
            return
        except Exception as ex:
            print(f'part {i} retry {attempt}: {ex}', flush=True)
            time.sleep(2)

open(OUT, 'wb').truncate(size)
ts = [threading.Thread(target=dl, args=(i,)) for i in range(N)]
[t.start() for t in ts]
for t in ts: t.join()
print('ALL PARTS DONE, final size:', os.path.getsize(OUT), flush=True)
