# ngx_brotli

Brotli is a generic-purpose lossless compression algorithm that compresses data
using a combination of a modern variant of the LZ77 algorithm, Huffman coding
and 2nd order context modeling, with a compression ratio comparable to the best
currently available general-purpose compression methods. It is similar in speed
with deflate but offers more dense compression.

ngx_brotli is a set of two nginx modules:

- ngx_brotli filter module - used to compress responses on-the-fly,
- ngx_brotli static module - used to serve pre-compressed files.


## Table of Contents

- [Status](#status)
- [Installation](#installation)
- [Configuration directives](#configuration-directives)
  - [`brotli_static`](#brotli_static)
  - [`brotli`](#brotli)
  - [`brotli_types`](#brotli_types)
  - [`brotli_comp_level`](#brotli_comp_level)
  - [`brotli_window`](#brotli_window)
  - [`brotli_min_length`](#brotli_min_length)
- [Sample configuration](#sample-configuration)
- [Tests](#tests)
- [Contributing](#contributing)
- [License](#license)

## Status

Both Brotli library and nginx module are under active development.

Three defaults have changed, which is worth knowing when upgrading. Set any of
them explicitly to keep the old behaviour.

Two went in favour of a smaller memory footprint. `brotli_window` is now `64k`
rather than `512k`, cutting per-request encoder memory by 58% on a response of
known length and 75% on a streamed one, at a cost in compression of 1.1% over
`script/corpus` and 2.4% at its worst when paired with the new quality default
below; and `brotli_min_length` is now `256` rather than `20`, so very small
responses are no longer compressed at all.

The third went in favour of latency. `brotli_comp_level` is now `4` rather
than `6`, which over the same corpus produces 7.4% more bytes for a little
over half the encoder time. This one does not affect memory.

`brotli_buffers` has been removed. It had been accepted and ignored for years,
and this module no longer has anything for it to configure: output is handed
straight out of the encoder's own buffer rather than copied into a pool of
them. Note that nginx treats an unknown directive as a fatal configuration
error, so a config still carrying `brotli_buffers` will refuse to start after
upgrading rather than warn — delete the line.

The `$brotli_ratio` variable has been removed. It reported the compression
ratio achieved for a request, which meant carrying two counters through the
body filter for the whole of every compressed response. There is no drop-in
replacement: `$body_bytes_sent` gives the compressed size, but the
uncompressed size it would be measured against is no longer available to the
log. This breaks the same way `brotli_buffers` does: nginx resolves variables
when it parses the configuration, so a `log_format` still naming
`$brotli_ratio` will refuse to start rather than log an empty field.

## Installation

### Statically compiled

Checkout the latest `ngx_brotli` and build the dependencies:

```
git clone --recurse-submodules -j8 https://github.com/google/ngx_brotli
cd ngx_brotli/deps/brotli
mkdir out && cd out
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_C_FLAGS="-Ofast -m64 -march=native -mtune=native -flto -funroll-loops -ffunction-sections -fdata-sections -Wl,--gc-sections" -DCMAKE_CXX_FLAGS="-Ofast -m64 -march=native -mtune=native -flto -funroll-loops -ffunction-sections -fdata-sections -Wl,--gc-sections" -DCMAKE_INSTALL_PREFIX=./installed ..
cmake --build . --config Release --target brotlienc
cd ../../../..
```


    $ cd nginx-1.x.x
    $ export CFLAGS="-m64 -march=native -mtune=native -Ofast -flto -funroll-loops -ffunction-sections -fdata-sections -Wl,--gc-sections"
    $ export LDFLAGS="-m64 -Wl,-s -Wl,-Bsymbolic -Wl,--gc-sections"
    $ ./configure --add-module=/path/to/ngx_brotli
    $ make && make install
  
This will compile the module directly into Nginx.


### Dynamically loaded

    $ cd nginx-1.x.x
    $ ./configure --with-compat --add-dynamic-module=/path/to/ngx_brotli
    $ make modules

You will need to use **exactly** the same `./configure` arguments as your Nginx configuration and append `--with-compat --add-dynamic-module=/path/to/ngx_brotli` to the end, otherwise you will get a "module is not binary compatible" error on startup. You can run `nginx -V` to get the configuration arguments for your Nginx installation.

`make modules` will result in `ngx_http_brotli_filter_module.so` and `ngx_http_brotli_static_module.so` in the `objs` directory. Copy these to `/usr/lib/nginx/modules/` then add the `load_module` directives to `nginx.conf` (above the http block):
```nginx
load_module modules/ngx_http_brotli_filter_module.so;
load_module modules/ngx_http_brotli_static_module.so;
```



## Configuration directives

### `brotli_static`

- **syntax**: `brotli_static on|off|always`
- **default**: `off`
- **context**: `http`, `server`, `location`

Enables or disables checking of the existence of pre-compressed files with`.br`
extension. With the `always` value, pre-compressed file is used in all cases,
without checking if the client supports it.

Serving a pre-compressed file is by far the cheapest option: no encoder is
created, so the request costs neither the compression CPU nor the encoder
memory that on-the-fly compression needs - measured against `script/corpus`,
0.56 MB for a small response and 1.9 MB for the larger ones, since a file has
a known length and so takes the more expensive of the two paths. Prefer it
wherever the content is static.

The catch is that every eligible request probes for `<path>.br`, and when that
file does not exist the probe reaches the filesystem **on every request**
unless negative results are cached. Caching them requires both directives:

```nginx
open_file_cache          max=1000 inactive=60s;
open_file_cache_errors   on;      # without this the .br miss is never cached
```

`open_file_cache` on its own is not enough - it caches the file that was found,
not the one that was missing. Set both if `brotli_static` is on in a location
where most files have no `.br` sibling.

### `brotli`

- **syntax**: `brotli on|off`
- **default**: `off`
- **context**: `http`, `server`, `location`, `if`

Enables or disables on-the-fly compression of responses.

Responses of any status are compressed, with the exception of those that carry
no body (`1xx`, `204`, `304`) or whose body is a byte range (`206`), since
labelling those with a `Content-Encoding` would corrupt the response.

#### Relationship to gzip, and what this module does not check

When this module takes a response it disables gzip for that request, so a
client advertising both encodings receives Brotli whatever relative weights it
gave them - `gzip;q=1.0, br;q=0.1` still yields Brotli.

Requests below HTTP/1.1 are never compressed, matching the `gzip_http_version`
default of `1.1`; `Vary: Accept-Encoding` is still advertised to them, so a
cache in front keeps the two answers apart. This is not configurable.

The consequence is easy to miss: **nginx's own compression controls do not
apply to Brotli, and suppressing gzip takes them out of play for that request
too.** This module decides using only `Accept-Encoding`, `brotli_types`,
`brotli_min_length` and the response status. In particular none of these have
any effect on it:

- `gzip_proxied`, which for requests arriving with a `Via` header can be told
  to leave alone responses carrying `Authorization`, `Cache-Control: no-cache`,
  `no-store` or `private`, `Expires`, `Last-Modified` or `ETag`. There is no
  `brotli_proxied`.
- `gzip_disable`, the user-agent escape hatch. There is no `brotli_disable`.

So if you rely on any of those to keep particular responses uncompressed -
authenticated or private responses passing through a shared proxy being the
usual case - turning `brotli on` in that location removes the protection
rather than mirroring it. Either leave `brotli` off there, or restrict it to
locations where compressing every eligible response is acceptable.

Worth remembering separately that compressing a response which mixes a secret
(a CSRF token, a session identifier) with attacker-influenced reflected input
leaks that secret through the response length, the BREACH attack. That applies
to any HTTP compression rather than to this module specifically, and neither
this module nor gzip mitigates it.

#### Proxied responses and time to first byte

Brotli accumulates roughly 64 KB of input before it emits anything, unless it
is explicitly told to flush. For a proxied response this interacts with
`proxy_buffering`, and the difference is large.

With `proxy_buffering on` (the default) nginx hands this filter buffers that
carry no flush marker, so the encoder waits for a full block before producing
output - and because nginx sends the response header together with the first
body write, the header waits too. Measured against an upstream trickling
240 KB as 8 KB every 50 ms, the client saw nothing for **813 ms**. The same
response uncompressed started arriving in 1 ms.

With `proxy_buffering off` every buffer is flush-marked, the filter compresses
and forwards it immediately, and time to first byte drops to **1 ms** for about
3.8% larger output.

Flushing that often also costs less memory, which is easy to miss. Smaller
meta-blocks mean a smaller working set inside the encoder: for the same 1.2 MB
response, peak encoder memory was 1817 KB with buffering on against 968 KB
with it off - a 47% reduction, for 2.2% larger output. The trade is a much
higher allocation count, roughly ten times as many, each correspondingly
smaller.

So `proxy_buffering off` is the better setting on both counts for proxied
traffic, and the difference is stark for slowly-produced responses -
server-sent events, long polling, progressive HTML. For responses that arrive
in one go it matters little, since a full block is available almost at once.

### `brotli_types`

- **syntax**: `brotli_types <mime_type> [..]`
- **default**: `text/html`
- **context**: `http`, `server`, `location`

Enables on-the-fly compression of responses for the specified MIME types
in addition to `text/html`. The special value `*` matches any MIME type.
Responses with the `text/html` MIME type are always compressed.

### `brotli_comp_level`

- **syntax**: `brotli_comp_level <level>`
- **default**: `4`
- **context**: `http`, `server`, `location`

Sets on-the-fly compression Brotli quality (compression) `level`.
Acceptable values are in the range from `0` to `11`.

The default trades compression for encoder time. Over `script/corpus`,
quality `4` produces 7.4% more bytes than `6` for a little over half the CPU.
Peak encoder memory does not change with this setting. If you serve mostly
cacheable assets, or sit behind a CDN, a higher level is likely the better
trade — `5` in particular costs only 0.8% in size against `6` while saving
20% of the time.

### `brotli_window`

- **syntax**: `brotli_window <size>`
- **default**: `64k`
- **context**: `http`, `server`, `location`

Sets Brotli window `size`. Acceptable values are `1k`, `2k`, `4k`, `8k`, `16k`,
`32k`, `64k`, `128k`, `256k`, `512k`, `1m`, `2m`, `4m`, `8m` and `16m`.

This is the single biggest influence on how much memory a request costs, but
not in a straight line: at `64k` and below Brotli selects a much cheaper
hasher, and above it encoder memory jumps sharply. Measured against
`script/corpus`, a streamed response peaks near 0.99 MB at `64k` against
4.0 MB at `512k`. `32k` and `16k` occupy the same memory as `64k` and only
compress worse, so there is little reason to go below it.

What the smaller window costs in compression depends entirely on the content,
because it only matters where a match would have reached back further than
64 KB:

| | `64k` | `512k` | cost |
|---|---:|---:|---:|
| CSS | 20,284 | 20,275 | +0.04% |
| JavaScript | 26,867 | 26,754 | +0.42% |
| minified JavaScript | 41,122 | 40,717 | +0.99% |
| prose | 94,715 | 92,707 | +2.17% |
| HTML | 29,339 | 28,556 | +2.74% |
| **whole corpus** | **212,327** | **209,009** | **+1.59%** |

Those figures are at `brotli_comp_level 6`. The cost falls as the quality
does, because a lower level finds fewer of the long-range matches a smaller
window gives up: at the default of `4` the same change costs `+1.07%` over the
corpus and `+2.44%` on its worst file.

Raise it if responses are large and bandwidth matters more than memory; a
window larger than the response body buys nothing. Note that when the response
length is known the module already lowers the window to fit, so this setting
mainly affects streamed responses and bodies larger than the window.

### `brotli_min_length`

- **syntax**: `brotli_min_length <length>`
- **default**: `256`
- **context**: `http`, `server`, `location`

Sets the minimum `length` of a response that will be compressed.
The length is determined only from the `Content-Length` response header field.

Compressing very small responses is counter-productive: the encoder costs
some 560 KB of memory no matter how little it is given, and below
about 128 bytes the compressed body plus the `Content-Encoding` header comes
out larger than the original. Note that a response of unknown length is
compressed regardless of this setting, since there is nothing to compare
against when the response headers are sent.

## Sample configuration

```
brotli on;
brotli_comp_level 4;
brotli_static on;

# brotli_static probes for <path>.br on every request; these two keep a
# missing sibling from reaching the filesystem each time.
open_file_cache        max=1000 inactive=60s;
open_file_cache_errors on;

brotli_types application/atom+xml application/javascript application/json application/vnd.api+json application/rss+xml
             application/vnd.ms-fontobject application/x-font-opentype application/x-font-truetype
             application/x-font-ttf application/x-javascript application/xhtml+xml application/xml
             font/eot font/opentype font/otf font/truetype image/svg+xml image/vnd.microsoft.icon
             image/x-icon image/x-win-bitmap text/css text/javascript text/plain text/xml;
```

## Tests

Two suites, both expecting an nginx binary built with this module. The same
scripts run in CI, so a green local run means a green pipeline:

```bash
script/build.sh                # Brotli library and CLI, then nginx
script/prepare-tests.sh        # fixtures for the shell suite
script/run-tests.sh            # static files and Accept-Encoding parsing
python3 script/test_stream.py  # streaming responses and encoder lifetime
```

`run-tests.sh` takes `NGINX_BIN` and `BROTLI` if you would rather point it at
binaries you already have. It is `NGINX_BIN` rather than `NGINX` because nginx
reserves the latter for socket inheritance and would read a path there as a
list of socket numbers.

`test_stream.py` covers what the shell suite does not: responses whose length
is unknown until the body has been streamed, and the lifetime of the Brotli
encoder instance, which owns heap memory that the request pool does not
release on its own. It starts its own nginx, generates its own fixtures and
runs its own stalling upstream, so it needs no setup beyond a binary:

```bash
python3 script/test_stream.py --nginx /path/to/nginx
```

It exits with the number of failed tests. The window and memory tests read the
encoder's allocator tracing out of the debug log, so build nginx with
`--with-debug` to run them - they are skipped, not failed, on a release build.
Round-trip tests need a brotli decoder, either the python `brotli` module or
the CLI that `script/build.sh` builds into `deps/brotli/out`.

### Corpus and benchmark

`script/corpus` holds real-world files - a rendered Wikipedia article, a
Tailwind stylesheet, React's development and production builds, and English
prose - checked in so the suite is deterministic offline. `test_stream.py`
round-trips each of them, over both the known-length and the unknown-length
path. Their provenance and licences are in
[`script/corpus/PROVENANCE.md`](script/corpus/PROVENANCE.md).

They exist because the generated fixtures are a poor stand-in for the web:
`make_text()` emits random words drawn from a small list, which repeat at long
range and so compress far better, and far more predictably, than real markup
or code. Measured against this corpus, a synthetic one overstated the cost of
a compression change roughly fourfold.

`script/bench_corpus.py` reports what the filter actually achieves on them:

```bash
python3 script/bench_corpus.py
python3 script/bench_corpus.py --quality 4,5,6 --window 16k,64k
```

At the shipped defaults, on a release build:

| file | raw | compressed | ratio |
|---|---:|---:|---:|
| `site.css` | 204,798 | 20,284 | 10.10x |
| `wiki.html` | 169,104 | 29,339 | 5.76x |
| `app.js` | 109,931 | 26,867 | 4.09x |
| `app.min.js` | 131,835 | 41,122 | 3.21x |
| `prose.txt` | 268,490 | 94,715 | 2.83x |
| **total** | **884,158** | **212,327** | **4.16x** |

The spread is the point: already-minified JavaScript compresses barely half as
well as the stylesheet, so a single headline ratio quoted from one file says
very little about the next.

It is a measurement tool rather than a test - a compression ratio has no pass
or fail, and the numbers move with the vendored Brotli version - so it asserts
nothing and CI does not run it. Reach for it when tuning a default, and put
the table in the commit message. Note that its timings are only meaningful on
a release build: a `--with-debug` nginx logs a line per encoder allocation,
which costs more than the compression being measured.

## Contributing

See [Contributing](CONTRIBUTING.md).

## License

    Copyright (C) 2002-2015 Igor Sysoev
    Copyright (C) 2011-2015 Nginx, Inc.
    Copyright (C) 2015 Google Inc.
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:
    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
    FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
    OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
    OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
    SUCH DAMAGE.
