"""Serve the WASM player on loopback with correct MIME types on Windows."""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import sys

class Handler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map,
        ".js": "text/javascript", ".mjs": "text/javascript", ".wasm": "application/wasm"}

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv)>1 else 8080
    server = ThreadingHTTPServer(("127.0.0.1", port), partial(Handler, directory=Path(__file__).resolve().parent))
    print(f"http://127.0.0.1:{server.server_port}/", flush=True)
    server.serve_forever()
