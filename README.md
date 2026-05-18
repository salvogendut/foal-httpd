![Example Image](foal-httpd.png)
# foal-httpd

Minimal HTTP/1.0 static file server for [SymbOS](http://symbos.org), written in C using the [SCC compiler](https://github.com/danielgaskell/scc).

> **Proof of concept.** This is a toy server intended for experimentation on retro hardware. It handles one connection at a time, has a fixed 8 KB file cache, and has not been hardened for security or reliability. Do not use it for anything serious.

## Features

- All files preloaded into RAM at startup — zero disk I/O during requests
- GET and HEAD methods
- MIME types: html, htm, txt, css, js, png, gif, jpg/jpeg
- Directory traversal protection (`..` in paths → 404)
- Query strings and fragments stripped before path lookup
- Default document: `index.htm` for `/`
- HTTP/1.0, `Connection: close` — one connection at a time

## Build

Requires SCC installed at `~/Dev/scc` (or set `SCC_HOME`):

```bash
./build.sh
```

Output: `build/httpd.com`

## Usage

```
httpd [port [docroot]]
```

| Argument | Default | Example |
|----------|---------|---------|
| `port` | 80 | `8080` |
| `docroot` | app directory | `C:\WWW` |

### Examples

```
httpd                    ; port 80, app directory
httpd 8080               ; port 8080, app directory
httpd 80 C:\WWW          ; port 80, serve from C:\WWW
```

Run from SymShell. The server preloads all known files at startup and logs each request to the shell. Press **Q** to quit (checked between connections).

## Preloaded files

At startup the server enumerates every file in the docroot with `Dir_ReadRaw` and caches them all in the 8 KB RAM pool. Any file present in the docroot directory is served; files not found there return 404. The pool fits roughly 8 KB of content in total (`STORE_SIZE` in `src/httpd.c`).

## Sample site

A minimal example site is in `www/`:

```
www/
  index.htm   — sample HTML page
  logo.jpg    — sample logo image
```

Copy the contents of `www/` to your SymbOS docroot (e.g. `C:\WWW\`) before starting the server.

## Notes

- Uses the SymbOS network daemon (must be running)
- SymbOS FAT paths use backslashes and 8.3 filenames (`C:\WWW\index.htm`)
- USB drive may sleep after idle; all files are read at startup to avoid hang
- Total RAM pool for cached files: 8 KB (`STORE_SIZE` in `src/httpd.c`)
