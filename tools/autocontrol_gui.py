import queue
import socket
import subprocess
import threading
import time
import tkinter as tk
from dataclasses import dataclass
from pathlib import Path
from tkinter import messagebox, ttk
from typing import Dict, Optional


APP_TITLE = "autoControl Tool"
DISCOVERY_PORT = 40000
CONTROL_PORT = 41000
MULTICAST_GROUP = "239.255.42.99"
DISCOVER_PAYLOAD = b"DISCOVER"


@dataclass
class Node:
    mac: str
    service: str
    cluster: str
    device: str
    ip: str
    port: int
    remote_ip: str
    remote_port: int
    last_seen: float


def parse_key_value_line(text: str) -> Dict[str, str]:
    data: Dict[str, str] = {}
    for part in text.strip().split():
      if "=" not in part:
        continue
      key, value = part.split("=", 1)
      data[key] = value
    return data


def parse_announce(text: str) -> Optional[Node]:
    data = parse_key_value_line(text)
    required = ["type", "service", "cluster", "device", "ip", "port", "mac"]
    if any(key not in data for key in required):
        return None
    if data["type"] != "announce":
        return None

    return Node(
        mac=data["mac"].lower(),
        service=data["service"],
        cluster=data["cluster"],
        device=data["device"],
        ip=data["ip"],
        port=int(data["port"]),
        remote_ip="",
        remote_port=0,
        last_seen=time.time(),
    )


class DiscoveryListener(threading.Thread):
    def __init__(self, bind_ip: str, port: int, output_queue: "queue.Queue[tuple[str, object]]"):
        super().__init__(daemon=True)
        self.bind_ip = bind_ip
        self.port = port
        self.output_queue = output_queue
        self.stop_event = threading.Event()
        self.sock: Optional[socket.socket] = None

    def stop(self) -> None:
        self.stop_event.set()
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass

    def run(self) -> None:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind((self.bind_ip, self.port))
            mreq = socket.inet_aton(MULTICAST_GROUP) + socket.inet_aton(
                self.bind_ip if self.bind_ip != "0.0.0.0" else "0.0.0.0"
            )
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            sock.settimeout(0.5)
            self.sock = sock
            self.output_queue.put(("log", f"discovery listener started on {self.bind_ip}:{self.port}"))
        except OSError as exc:
            self.output_queue.put(("error", f"discovery listener start failed: {exc}"))
            return

        while not self.stop_event.is_set():
            try:
                data, remote = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break

            text = data.decode("ascii", errors="replace").strip()
            node = parse_announce(text)
            if node is None:
                self.output_queue.put(("log", f"udp {remote[0]}:{remote[1]} -> {text}"))
                continue

            node.remote_ip = remote[0]
            node.remote_port = remote[1]
            node.last_seen = time.time()
            self.output_queue.put(("node", node))
            self.output_queue.put(("log", f"announce {remote[0]}:{remote[1]} -> {text}"))

        self.output_queue.put(("log", "discovery listener stopped"))


class AutoControlApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title(APP_TITLE)
        self.root.geometry("980x700")

        self.events: "queue.Queue[tuple[str, object]]" = queue.Queue()
        self.listener: Optional[DiscoveryListener] = None
        self.nodes: Dict[str, Node] = {}

        self.target_ip_var = tk.StringVar(value="192.168.0.218")
        self.control_port_var = tk.StringVar(value=str(CONTROL_PORT))
        self.discovery_port_var = tk.StringVar(value=str(DISCOVERY_PORT))
        self.bind_ip_var = tk.StringVar(value="0.0.0.0")
        self.status_var = tk.StringVar(value="ready")
        self.relay_vars = [tk.IntVar(value=0) for _ in range(7)]

        self._build_ui()
        self._start_listener()
        self.root.after(200, self._pump_events)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self) -> None:
        main = ttk.Frame(self.root, padding=12)
        main.pack(fill=tk.BOTH, expand=True)

        top = ttk.LabelFrame(main, text="Target")
        top.pack(fill=tk.X)

        ttk.Label(top, text="IP").grid(row=0, column=0, padx=6, pady=6, sticky=tk.W)
        ttk.Entry(top, textvariable=self.target_ip_var, width=18).grid(row=0, column=1, padx=6, pady=6)
        ttk.Label(top, text="Control Port").grid(row=0, column=2, padx=6, pady=6, sticky=tk.W)
        ttk.Entry(top, textvariable=self.control_port_var, width=8).grid(row=0, column=3, padx=6, pady=6)
        ttk.Label(top, text="Discovery Port").grid(row=0, column=4, padx=6, pady=6, sticky=tk.W)
        ttk.Entry(top, textvariable=self.discovery_port_var, width=8).grid(row=0, column=5, padx=6, pady=6)
        ttk.Label(top, text="Bind").grid(row=0, column=6, padx=6, pady=6, sticky=tk.W)
        ttk.Entry(top, textvariable=self.bind_ip_var, width=14).grid(row=0, column=7, padx=6, pady=6)

        actions = ttk.Frame(top)
        actions.grid(row=1, column=0, columnspan=8, padx=6, pady=(0, 8), sticky=tk.W)
        ttk.Button(actions, text="Discover", command=self.send_discover).pack(side=tk.LEFT, padx=(0, 8))
        ttk.Button(actions, text="ICMP Ping", command=self.os_ping).pack(side=tk.LEFT, padx=(0, 8))
        ttk.Button(actions, text="TCP PING", command=self.control_ping).pack(side=tk.LEFT, padx=(0, 8))
        ttk.Button(actions, text="GET RELAY", command=self.get_relay).pack(side=tk.LEFT, padx=(0, 8))
        ttk.Button(actions, text="Restart Listen", command=self._restart_listener).pack(side=tk.LEFT, padx=(0, 8))

        middle = ttk.PanedWindow(main, orient=tk.VERTICAL)
        middle.pack(fill=tk.BOTH, expand=True, pady=(12, 0))

        nodes_frame = ttk.LabelFrame(middle, text="Discovery Nodes")
        relay_frame = ttk.LabelFrame(middle, text="Relay Control")
        log_frame = ttk.LabelFrame(middle, text="Log")

        middle.add(nodes_frame, weight=3)
        middle.add(relay_frame, weight=2)
        middle.add(log_frame, weight=3)

        self.node_tree = ttk.Treeview(
            nodes_frame,
            columns=("ip", "device", "service", "cluster", "mac", "seen"),
            show="headings",
            height=8,
        )
        for key, title, width in [
            ("ip", "IP", 120),
            ("device", "Device", 120),
            ("service", "Service", 120),
            ("cluster", "Cluster", 120),
            ("mac", "MAC", 150),
            ("seen", "Last Seen", 140),
        ]:
            self.node_tree.heading(key, text=title)
            self.node_tree.column(key, width=width, anchor=tk.W)
        self.node_tree.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)
        self.node_tree.bind("<<TreeviewSelect>>", self._on_tree_select)

        relay_buttons = ttk.Frame(relay_frame, padding=8)
        relay_buttons.pack(fill=tk.X)
        for index, relay_var in enumerate(self.relay_vars, start=1):
            ttk.Checkbutton(relay_buttons, text=f"CH{index}", variable=relay_var).pack(side=tk.LEFT, padx=6, pady=6)

        relay_actions = ttk.Frame(relay_frame, padding=(8, 0, 8, 8))
        relay_actions.pack(fill=tk.X)
        ttk.Button(relay_actions, text="Apply Mask", command=self.apply_relay_mask).pack(side=tk.LEFT, padx=(0, 8))
        ttk.Button(relay_actions, text="All On", command=lambda: self.set_all_relays(1)).pack(side=tk.LEFT, padx=(0, 8))
        ttk.Button(relay_actions, text="All Off", command=lambda: self.set_all_relays(0)).pack(side=tk.LEFT, padx=(0, 8))
        ttk.Button(relay_actions, text="Refresh", command=self.get_relay).pack(side=tk.LEFT, padx=(0, 8))

        self.log_text = tk.Text(log_frame, wrap="word", height=14)
        self.log_text.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)
        self.log_text.configure(state=tk.DISABLED)

        status_bar = ttk.Label(main, textvariable=self.status_var, anchor=tk.W)
        status_bar.pack(fill=tk.X, pady=(8, 0))

    def _append_log(self, message: str) -> None:
        timestamp = time.strftime("%H:%M:%S")
        self.log_text.configure(state=tk.NORMAL)
        self.log_text.insert(tk.END, f"[{timestamp}] {message}\n")
        self.log_text.see(tk.END)
        self.log_text.configure(state=tk.DISABLED)
        self.status_var.set(message)

    def _start_listener(self) -> None:
        if self.listener is not None:
            return
        self.listener = DiscoveryListener(self.bind_ip_var.get().strip(), int(self.discovery_port_var.get()), self.events)
        self.listener.start()

    def _restart_listener(self) -> None:
        if self.listener is not None:
            self.listener.stop()
            self.listener = None
        self._start_listener()

    def _on_close(self) -> None:
        if self.listener is not None:
            self.listener.stop()
        self.root.destroy()

    def _pump_events(self) -> None:
        while True:
            try:
                kind, payload = self.events.get_nowait()
            except queue.Empty:
                break

            if kind == "log":
                self._append_log(str(payload))
            elif kind == "error":
                self._append_log(str(payload))
                messagebox.showerror(APP_TITLE, str(payload))
            elif kind == "node":
                self._update_node(payload)
            elif kind == "relay":
                self._apply_relay_response(payload)
            elif kind == "response":
                self._append_log(str(payload))
            elif kind == "set_ip":
                self.target_ip_var.set(str(payload))
            elif kind == "ping":
                self._append_log(str(payload))

        self.root.after(200, self._pump_events)

    def _update_node(self, node: Node) -> None:
        self.nodes[node.mac] = node
        item_id = node.mac
        values = (
            node.ip,
            node.device,
            node.service,
            node.cluster,
            node.mac,
            time.strftime("%H:%M:%S", time.localtime(node.last_seen)),
        )
        if self.node_tree.exists(item_id):
            self.node_tree.item(item_id, values=values)
        else:
            self.node_tree.insert("", tk.END, iid=item_id, values=values)

    def _on_tree_select(self, _event: object) -> None:
        selection = self.node_tree.selection()
        if not selection:
            return
        node = self.nodes.get(selection[0])
        if node is not None:
            self.target_ip_var.set(node.ip)
            self._append_log(f"selected node {node.device} {node.ip}")

    def _run_background(self, title: str, func) -> None:
        def worker() -> None:
            try:
                func()
            except Exception as exc:  # pragma: no cover - runtime safety
                self.events.put(("error", f"{title} failed: {exc}"))

        threading.Thread(target=worker, daemon=True).start()

    def send_discover(self) -> None:
        def work() -> None:
            port = int(self.discovery_port_var.get())
            payload = DISCOVER_PAYLOAD

            for destination in [("255.255.255.255", port), (MULTICAST_GROUP, port)]:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP) as sock:
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                    sock.settimeout(1.0)
                    sock.sendto(payload, destination)
                self.events.put(("log", f"discover sent to {destination[0]}:{destination[1]}"))

        self._run_background("discover", work)

    def os_ping(self) -> None:
        def work() -> None:
            ip = self.target_ip_var.get().strip()
            if not ip:
                raise ValueError("IP is empty")

            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            completed = subprocess.run(
                ["ping", "-n", "1", ip],
                capture_output=True,
                text=True,
                timeout=8,
                startupinfo=startupinfo,
            )
            summary = completed.stdout.strip().splitlines()[-2:] if completed.stdout.strip() else [completed.stderr.strip()]
            self.events.put(("ping", " | ".join(line.strip() for line in summary if line.strip())))

        self._run_background("icmp ping", work)

    def _tcp_request(self, request_text: str) -> str:
        ip = self.target_ip_var.get().strip()
        port = int(self.control_port_var.get().strip())
        if not ip:
            raise ValueError("IP is empty")

        with socket.create_connection((ip, port), timeout=3.0) as sock:
            sock.sendall((request_text + "\r\n").encode("ascii"))
            sock.settimeout(3.0)
            data = sock.recv(1024)
        return data.decode("ascii", errors="replace").strip()

    def control_ping(self) -> None:
        def work() -> None:
            response = self._tcp_request("PING")
            self.events.put(("response", f"TCP PING -> {response}"))

        self._run_background("tcp ping", work)

    def get_relay(self) -> None:
        def work() -> None:
            response = self._tcp_request("GET RELAY")
            self.events.put(("response", f"GET RELAY -> {response}"))
            self.events.put(("relay", response))

        self._run_background("get relay", work)

    def apply_relay_mask(self) -> None:
        def work() -> None:
            mask = 0
            for index, relay_var in enumerate(self.relay_vars):
                if relay_var.get():
                    mask |= (1 << index)
            response = self._tcp_request(f"SET RELAY MASK {mask}")
            self.events.put(("response", f"SET RELAY MASK {mask} -> {response}"))
            self.events.put(("relay", response))

        self._run_background("set relay mask", work)

    def set_all_relays(self, value: int) -> None:
        for relay_var in self.relay_vars:
            relay_var.set(value)
        self.apply_relay_mask()

    def _apply_relay_response(self, response: str) -> None:
        data = parse_key_value_line(response)
        if data.get("type") != "relay":
            return

        for index in range(7):
            key = f"ch{index + 1}"
            if key in data:
                self.relay_vars[index].set(1 if data[key] == "1" else 0)


def main() -> int:
    root = tk.Tk()
    icon_path = Path(__file__).with_suffix(".ico")
    if icon_path.exists():
        try:
            root.iconbitmap(default=str(icon_path))
        except tk.TclError:
            pass
    AutoControlApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
