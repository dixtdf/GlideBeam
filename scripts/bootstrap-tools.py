import hashlib, json, os, pathlib, time, urllib.request, zipfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
TOOLS.mkdir(exist_ok=True)
proxy = urllib.request.build_opener(urllib.request.ProxyHandler({'http':'http://127.0.0.1:10808','https':'http://127.0.0.1:10808'}))
direct = urllib.request.build_opener(urllib.request.ProxyHandler({}))
def fetch(url, destination=None):
    last = None
    for attempt in range(4):
        try:
            opener = proxy if attempt < 2 else direct
            req = urllib.request.Request(url,headers={'User-Agent':'RemoteNative-build/0.1'})
            with opener.open(req,timeout=90) as response:
                if destination:
                    temporary = destination.with_suffix(destination.suffix+'.part')
                    with temporary.open('wb') as output:
                        count = 0
                        while True:
                            chunk = response.read(1024*1024)
                            if not chunk: break
                            output.write(chunk); count += len(chunk)
                        print(f'Downloaded {destination.name}: {count} bytes',flush=True)
                    temporary.replace(destination)
                    return destination
                return response.read()
        except Exception as error:
            last = error
            print(f'Retry {attempt+1}: {type(error).__name__}',flush=True)
            time.sleep(1)
    raise last
release = json.loads(fetch('https://api.github.com/repos/brechtsanders/winlibs_mingw/releases/latest'))
assets = [a for a in release['assets'] if 'x86_64' in a['name'] and a['name'].endswith('.zip') and 'llvm' not in a['name']]
if not assets: raise RuntimeError('No x86_64 GCC archive in official release')
asset = min(assets,key=lambda a:a['size'])
archive = TOOLS / asset['name']
if not archive.exists(): fetch(asset['browser_download_url'],archive)
digest = asset.get('digest')
if digest and digest.startswith('sha256:'):
    actual = hashlib.file_digest(archive.open('rb'),'sha256').hexdigest()
    if actual != digest.split(':',1)[1]: raise RuntimeError('Compiler SHA256 mismatch')
if not (TOOLS/'mingw64'/'bin'/'g++.exe').exists():
    with zipfile.ZipFile(archive) as package:
        for entry in package.infolist():
            target = (TOOLS/entry.filename).resolve()
            if not target.is_relative_to(TOOLS.resolve()): raise RuntimeError('Archive path escapes tools')
        package.extractall(TOOLS)
print(f'GCC ready: {TOOLS / "mingw64" / "bin" / "g++.exe"}',flush=True)
rustup = TOOLS / 'rustup-init.exe'
if not rustup.exists(): fetch('https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-gnu/rustup-init.exe',rustup)
print('Rustup installer ready',flush=True)
(TOOLS/'sources.json').write_text(json.dumps({'gcc_release':release['tag_name'],'gcc_asset':asset['browser_download_url'],'gcc_digest':digest,'rustup_source':'https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-gnu/rustup-init.exe'},indent=2),encoding='utf-8')
