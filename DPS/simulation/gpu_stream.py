import argparse
import csv
import json
import os
import re
import threading
import time
import http.server
import socketserver
from pathlib import Path

LEADER_RE = re.compile(r"Leader: POS\(([-\d\.]+),([-\d\.]+)\) SPD=([-\d\.]+)")
FOLLOWER_RE = re.compile(  
    r"\[STATE: ([^\]]+)\] \[POS: ([-\d\.]+),([-\d\.]+)\] \[SPD: ([-\d\.]+)\](?: \[DIR: [^\]]+\])? \[GAP: ([-\d\.]+)\]"
)
FOLLOWER_SHUTDOWN_RE = re.compile(r"\[FOLLOWER\] Shutdown requested .*tcp recv closed")
LEADER_DISC_RE = re.compile(r"Leader is disconnected\.")


def tail_file(path):
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        f.seek(0, os.SEEK_END)
        buf = ""
        while True:
            chunk = f.read()
            if not chunk:
                time.sleep(0.05)
                continue
            chunk = chunk.replace("\r", "\n")
            buf += chunk
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                if line:
                    yield line


def main():
    parser = argparse.ArgumentParser(description="Stream follower/leader logs to JSON/CSV for Colab GPU mirror.")
    parser.add_argument("--leader-log", required=True, help="Path to leader.log")
    parser.add_argument("--follower-log", action="append", required=True, help="Follower log entry: id:path")
    parser.add_argument("--out", default="latest.json", help="Output JSON path (latest sample)")
    parser.add_argument("--csv", default="stream.csv", help="Append CSV output path")
    parser.add_argument("--intruder-out", default="intruder_{id}.txt", help="Intruder file pattern (written by /intruder POST)")
    parser.add_argument("--follow-id", type=int, default=None, help="Follower ID when using single --follower-log")
    parser.add_argument("--serve", action="store_true", help="Serve output dir via HTTP")
    parser.add_argument("--port", type=int, default=8000, help="HTTP port when --serve is set")
    args = parser.parse_args()

    leader_state = {"x": None, "y": None, "spd": None, "ts": None}
    last_samples = {}
    lock = threading.Lock()

    out_path = Path(args.out).resolve()
    csv_path = Path(args.csv).resolve()
    intruder_pattern = args.intruder_out

    followers = []
    for entry in args.follower_log:
        if ":" in entry:
            fid_s, path = entry.split(":", 1)
            try:
                fid = int(fid_s)
            except ValueError:
                raise SystemExit(f"Invalid follower id in '{entry}'")
            followers.append((fid, path))
        else:
            if args.follow_id is None:
                raise SystemExit("Use --follow-id with single --follower-log or pass id:path")
            followers.append((args.follow_id, entry))

    def update_leader_disconnected():
        now = time.time()
        with lock:
            for fid, last in list(last_samples.items()):
                sample = dict(last)
                sample["disconnected"] = True
                sample["ts"] = now
                last_samples[fid] = sample
        write_latest_aggregate()

    def update_leader(line):  
        m = LEADER_RE.search(line)
        if not m:
            return
        x, y, spd = map(float, m.groups())
        with lock:
            leader_state["x"] = x
            leader_state["y"] = y
            leader_state["spd"] = spd
            leader_state["ts"] = time.time()

    def write_latest_aggregate():
        payload = {"ts": time.time(), "followers": {}}  
        with lock:
            for fid, sample in last_samples.items():
                payload["followers"][str(fid)] = sample
        tmp = out_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(payload))
        tmp.replace(out_path)

    def write_intruder_line(target_id, line):
        out_path_local = intruder_pattern.format(id=target_id)
        out_path_local = Path(out_path_local).resolve()
        tmp = out_path_local.with_suffix(".tmp")
        tmp.write_text(line)
        tmp.replace(out_path_local)

    def emit_sample(sample):
        fid = sample["follower_id"]
        with lock:
            last_samples[fid] = sample
        write_latest_aggregate()
        write_header = not csv_path.exists() or csv_path.stat().st_size == 0
        with open(csv_path, "a", newline="") as f:
            w = csv.writer(f)
            if write_header:
                w.writerow([
                    "ts",
                    "follower_id",
                    "state",
                    "my_x",
                    "my_y",
                    "current_speed",
                    "gap",
                    "leader_x",
                    "leader_y",
                    "leader_speed",
                ])
            w.writerow([
                sample["ts"],
                sample["follower_id"],
                sample["state"],
                sample["my_x"],
                sample["my_y"],
                sample["current_speed"],
                sample["gap"],
                sample["leader_x"],
                sample["leader_y"],
                sample["leader_speed"],
            ])

    def update_follower(line, fid):
        if LEADER_DISC_RE.search(line):
            update_leader_disconnected()
            return
        if FOLLOWER_SHUTDOWN_RE.search(line):
            now = time.time()
            with lock:
                last = last_samples.get(fid, {"follower_id": fid})
                sample = dict(last)
                sample["disconnected"] = True
                sample["ts"] = now
                last_samples[fid] = sample
            write_latest_aggregate()
            return
        m = FOLLOWER_RE.search(line)
        if not m:
            return
        state, my_x_s, my_y_s, cur_spd_s, gap_s = m.groups()
        my_x = float(my_x_s)
        my_y = float(my_y_s)
        cur_spd = float(cur_spd_s)
        gap = float(gap_s)

        with lock:
            lx = leader_state["x"]
            ly = leader_state["y"]
            lspd = leader_state["spd"]

        if lx is None or ly is None or lspd is None:
            return

        sample = {
            "ts": time.time(),
            "follower_id": fid,
            "state": state,
            "my_x": my_x,
            "my_y": my_y,
            "current_speed": cur_spd,
            "gap": gap,
            "leader_x": lx,
            "leader_y": ly,
            "leader_speed": lspd,
            "disconnected": False,
        }
        emit_sample(sample)

    def leader_thread():
        for line in tail_file(args.leader_log):
            update_leader(line)

    def follower_thread(fid, path):
        for line in tail_file(path):
            update_follower(line, fid)

    t1 = threading.Thread(target=leader_thread, daemon=True)
    t1.start()
    follower_threads = []
    for fid, path in followers:
        t = threading.Thread(target=follower_thread, args=(fid, path), daemon=True)
        t.start()
        follower_threads.append(t)

    if args.serve:
        serve_dir = str(out_path.parent)
        os.chdir(serve_dir)
        socketserver.ThreadingTCPServer.allow_reuse_address = True
        intruder_lock = threading.Lock()

        class Handler(http.server.SimpleHTTPRequestHandler):  
            def do_POST(self):
                if self.path != "/intruder":
                    self.send_error(404, "Not Found")
                    return
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length).decode("utf-8", errors="ignore")
                try:
                    data = json.loads(body)
                except json.JSONDecodeError:
                    self.send_error(400, "Invalid JSON")
                    return

                seq = int(data.get("seq", int(time.time() * 1000)))
                target_id = int(data.get("target_id", 0))
                active = 1 if data.get("active") else 0
                speed = int(data.get("speed", 0))
                intr_len = int(data.get("length", 0))
                duration_ms = int(data.get("duration_ms", 0))
                ts = time.time()

                line = f"{seq} {target_id} {active} {speed} {intr_len} {duration_ms} {ts}\n"
                with intruder_lock:
                    write_intruder_line(target_id, line)

                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b"{\"ok\": true}")

        httpd = socketserver.ThreadingTCPServer(("0.0.0.0", args.port), Handler)
        print(f"Serving {serve_dir} on http://0.0.0.0:{args.port}")
        httpd.serve_forever()
    else:
        while True:
            time.sleep(1)


if __name__ == "__main__":
    main()
