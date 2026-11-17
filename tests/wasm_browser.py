"""Browser integration check: generated PCM fixture, optionally a user's real PAKs."""
import argparse
import http.server
from pathlib import Path
import tempfile
import threading
import wave
from functools import partial
from playwright.sync_api import sync_playwright

parser = argparse.ArgumentParser()
parser.add_argument("--pak-dir", type=Path)
parser.add_argument("--browser", help="Chromium executable (otherwise Playwright default)")
args = parser.parse_args()
site = Path(__file__).resolve().parents[1] / "wasm"
import sys
sys.path.insert(0,str(site))
from serve import Handler
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler, directory=site))
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory() as tmp, sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=args.browser)
        page = browser.new_page(viewport={"width": 390, "height": 844}, has_touch=True)
        errors = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto(f"http://127.0.0.1:{server.server_port}")
        page.wait_for_function("!document.querySelector('#files').disabled")
        assert page.evaluate("document.documentElement.scrollWidth <= innerWidth")
        fixture = Path(tmp) / "mono.wav"
        with wave.open(str(fixture), "wb") as wav:
            wav.setparams((1, 2, 22050, 0, "NONE", ""))
            wav.writeframes((b'\x00\x40' * 8 + b'\x00\xc0' * 8) * 12000)
        page.locator("#files").set_input_files(str(fixture))
        page.wait_for_function("!document.querySelector('#play').disabled")
        page.locator("#play").click()
        page.wait_for_function("Number(document.querySelector('#seek').value) > 0")
        page.locator("#play").click()
        page.locator("#seek").fill("650")
        page.locator("#play").click()
        page.wait_for_function("Number(document.querySelector('#seek').value) > 650")
        page.locator("#stop").click()
        assert page.locator("#seek").is_disabled()
        if args.pak_dir:
            files = [p for p in args.pak_dir.glob("*.pak") if p.name.lower() in {"bbt.pak","bbt2.pak","bbt3.pak","bbll.pak"}]
            page.locator("#files").set_input_files([str(p.resolve()) for p in files])
            page.wait_for_function("document.querySelector('#status').textContent.includes('46/46')")
            page.locator("#tracks").select_option("0")
            page.locator("#play").click()
            page.wait_for_function("Number(document.querySelector('#seek').value) > 0")
            assert page.locator("#status").inner_text() == "graveyard"
            print("PAK: 46/46; TR:00 decoded and playback clock advanced")
        assert not errors, errors
        browser.close()
        print("PASS: small viewport, import, decode, play, pause, seek, stop")
finally:
    server.shutdown()
    server.server_close()
