![Example Image](foal-httpd.png)
# foal-httpd

Minimal HTTP/1.0 static file server for [SymbOS](http://symbos.org), written in C using the [SCC compiler](https://github.com/danielgaskell/scc).

> **Proof of concept.** This is a toy server intended for experimentation on retro hardware. It handles one connection at a time, has a fixed 16 KB file cache, and has not been hardened for security or reliability. Do not use it for anything serious.

## Features

- All files in the docroot preloaded into RAM at startup — zero disk I/O during requests
- GET and HEAD methods
- MIME types: html, htm, txt, css, js, png, gif, jpg/jpeg
- Directory traversal protection (`..` in paths → 404)
- Query strings and fragments stripped before path lookup
- Default document: `index.htm` for `/`
- HTTP/1.0, `Connection: close` — one connection at a time; see [Limitations](#limitations) below
- Runs as a SymbOS daemon with a systray icon
- Double-launch guard (second instance exits immediately)
- Responds to OS quit messages (task manager) and Q key in SymShell

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

Run from SymShell or the SymbOS file manager. On startup the server scans the docroot, caches all files, registers itself as a named service, and adds a systray icon. It then waits for connections in the background.

- **SymShell**: press **Q** to quit (checked between connections)
- **No shell / daemon mode**: quit via the SymbOS task manager or OS quit message

## File caching

At startup the server enumerates every file in the docroot using the SymbOS file manager (DIRINP command), caches them all into a 16 KB RAM pool, then closes all file handles. Any file present at startup is served from RAM; files not found return 404. The pool fits roughly 16 KB of content in total (`STORE_SIZE` in `src/httpd.c`).

## Limitations

This server handles **one connection at a time**. After serving a response it closes the socket and begins listening again. Modern browsers open multiple parallel connections to fetch a page's assets (images, CSS, JS), and those secondary requests arrive while the server is still processing the first one — they get `ERR_CONNECTION_REFUSED`.

**Consequence: images and other assets must be embedded directly in the HTML** as base64 data URIs rather than referenced as separate URLs. The browser then gets everything it needs from the single index page response.

```html
<!-- Instead of: <img src="logo.jpg"> -->
<img src="data:image/jpeg;base64,/9j/4AAQ..." alt="logo">
```

To convert an image on the host machine:

```bash
python3 -c "
import base64
with open('logo.jpg','rb') as f:
    print('data:image/jpeg;base64,' + base64.b64encode(f.read()).decode())
"
```

Paste the output as the `src` attribute value.

## Sample site

A minimal example site is in `www/`:

```
www/
  index.htm   — sample HTML page with logo embedded as a base64 data URI
```

Copy `www/index.htm` to your SymbOS docroot (e.g. `C:\WWW\`) before starting the server.

## Notes

- Requires the SymbOS network daemon to be running
- SymbOS FAT paths use backslashes and 8.3 filenames (`C:\WWW\index.htm`)
- All files are read at startup; the USB drive may sleep safely during serving
- Total RAM pool for cached files: 16 KB (`STORE_SIZE` in `src/httpd.c`)
