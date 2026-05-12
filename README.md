# foal-httpd

Minimal HTTP/1.0 static file server for [SymbOS](http://symbos.org), written in C using the [SCC compiler](https://github.com/danielgaskell/scc).

## Features

- Serves static files from a configurable document root
- GET and HEAD methods
- MIME types: html, htm, txt, css, js, png, gif, jpg/jpeg
- Directory traversal protection (`..` in paths → 404)
- Query strings and fragments stripped before path lookup
- Default document: `index.html` for `/`
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
| `docroot` | current directory | `A:\WWW` |

### Examples

```
httpd                    ; port 80, current directory
httpd 8080               ; port 8080, current directory
httpd 80 A:\WWW          ; port 80, serve from A:\WWW
```

Run from SymShell. The server prints the address it is listening on, then logs each request to the shell.

## Notes

- Uses the SymbOS network daemon (must be running)
- SymbOS paths use backslashes (`A:\WWW\index.html`)
- URL paths (`/subdir/page.html`) are converted automatically
- Files are read via the SymbOS file API; the FAT drive must be accessible
- Optimised for low memory: request buffer 512 B, file chunk 512 B
