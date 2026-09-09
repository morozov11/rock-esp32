#!/usr/bin/env python3
import http.server
import socketserver
import threading
import time

HTTP_PORT = 8080

class MockAudioHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.endswith('.mp3'):
            self.send_response(200)
            self.send_header('Content-Type', 'audio/mpeg')
            try:
                with open('managed_components/espressif__esp_audio_codec/test_apps/audio_codec_test/main/test.mp3', 'rb') as f:
                    data = f.read()
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            except Exception as e:
                self.send_error(404, str(e))
        else:
            self.send_error(404)

def run_http():
    with socketserver.TCPServer(('0.0.0.0', HTTP_PORT), MockAudioHandler) as httpd:
        print(f'[Mock HTTP] Serving MP3 on port {HTTP_PORT}')
        httpd.serve_forever()

if __name__ == '__main__':
    t = threading.Thread(target=run_http, daemon=True)
    t.start()
    print(f'[Mock Player Server] Ready for testing on HTTP {HTTP_PORT}')
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
