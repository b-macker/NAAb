# Vigilant/analyst/entropy_engine.py
# PHASE 2 (v3.0): RESILIENT SWARM ANALYST
# Features: Threaded Concurrency, Watchdog Heartbeat

import socketserver
import os
import math
import json

class AnalystHandler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            data = self.request.recv(1024*128).decode('utf-8')
            if not data: return
            
            # Watchdog Heartbeat
            if data == "PING":
                self.request.sendall(b"PONG")
                return

            findings = self.analyze(data)
            self.request.sendall(json.dumps(findings).encode('utf-8'))
        except Exception as e:
            print(f"[ANALYST ERROR] {e}")

    def calculate_entropy(self, s):
        if not s: return 0
        prob = [float(s.count(c)) / len(s) for c in dict.fromkeys(list(s))]
        return - sum([p * math.log(p, 2) for p in prob])

    def analyze(self, text):
        findings = []
        for word in text.split():
            clean = word.strip('"\',:;()[]{}')
            if len(clean) > 20:
                ent = self.calculate_entropy(clean)
                if ent > 3.8:
                    findings.append({"type": "SEC_HIGH_ENTROPY", "score": round(ent, 2)})
        return findings

SOCKET_PATH = "/data/data/com.termux/files/usr/tmp/v_a.sock"

if __name__ == "__main__":
    if os.path.exists(SOCKET_PATH): os.remove(SOCKET_PATH)
    
    with socketserver.ThreadingUnixStreamServer(SOCKET_PATH, AnalystHandler) as server:
        server.serve_forever()
