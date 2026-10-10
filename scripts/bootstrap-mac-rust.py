import hashlib, pathlib, tomllib
from urllib.request import ProxyHandler, build_opener, Request
ROOT=pathlib.Path(__file__).resolve().parents[1]
OUT=ROOT/'tools'/'macos-rust'
OUT.mkdir(parents=True,exist_ok=True)
opener=build_opener(ProxyHandler({'http':'http://127.0.0.1:10808','https':'http://127.0.0.1:10808'}))
manifest=tomllib.loads(opener.open(Request('https://static.rust-lang.org/dist/channel-rust-stable.toml',headers={'User-Agent':'RemoteNative-build'}),timeout=90).read().decode())
for name in ('rustc','rust-std','cargo'):
    record=manifest['pkg'][name]['target']['x86_64-apple-darwin']
    url=record['xz_url']; expected=record['xz_hash']; destination=OUT/url.rsplit('/',1)[-1]
    if not destination.exists():
        with opener.open(url,timeout=180) as response,destination.with_suffix('.part').open('wb') as file:
            while chunk:=response.read(1024*1024):file.write(chunk)
        destination.with_suffix('.part').replace(destination)
    with destination.open('rb') as file:actual=hashlib.file_digest(file,'sha256').hexdigest()
    if actual!=expected:raise RuntimeError(f'{name} SHA256 mismatch')
    print(f'{name}: {destination.name} {destination.stat().st_size} bytes SHA256 verified',flush=True)
