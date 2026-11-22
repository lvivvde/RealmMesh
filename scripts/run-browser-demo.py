#!/usr/bin/env python3
"""启动仅监听本机的双人演示；Ctrl-C 后回收本次启动的子进程。"""
import argparse
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DEMO = ROOT / "tools/browser-demo"


def configure(directory):
    config = directory / "configs"
    shutil.copytree(ROOT / "configs", config)
    changes = {
        "main.config": [('        { name = "login_verify" },', '        { name = "scene" },\n        { name = "login_verify" },')],
        "common/discovery.lua": [("enabled = false", "enabled = true"), (":2379", ":12379")],
        "services/login_verify.lua": [("listen_port = 0,", "listen_port = 17443,")],
        "services/queue.lua": [("listen_port = 0,", "listen_port = 17444,"), (":2379", ":12379")],
        "services/realm.lua": [("enabled = false", "enabled = true"), ('"0.0.0.0"', '"127.0.0.1"')],
        "services/gateway.lua": [('protocol = "quic",\n            enabled = true', 'protocol = "quic",\n            enabled = false'), ('"0.0.0.0"', '"127.0.0.1"')],
    }
    for name, substitutions in changes.items():
        path = config / name
        source = path.read_text()
        for previous, replacement in substitutions:
            if previous not in source:
                raise RuntimeError(f"演示配置与 {name} 不匹配：{previous}")
            source = source.replace(previous, replacement)
        path.write_text(source)
    return config


def service_environment(directory):
    tls = directory / "tls"
    subprocess.run(["bash", ROOT / "tests/scripts/generate_service_test_certs.sh", tls, "openssl"], check=True)
    subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                    "-nodes", "-keyout", tls / "edge.key", "-out", tls / "edge.pem", "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    material = json.loads(subprocess.check_output(["node", "-e", """
const pair = require('node:crypto').generateKeyPairSync('ed25519');
const {d,x} = pair.privateKey.export({format:'jwk'});
console.log(JSON.stringify([d,x].map(v=>Buffer.from(v,'base64url').toString('hex'))))
"""]))
    env = dict(os.environ, REALMMESH_ADMISSION_GRANT_KEY_SEED=material[0],
               REALMMESH_ADMISSION_GRANT_PUBLIC_KEY=material[1])
    for name in ["IDENTITY_KEY_SEED", "QUEUE_NUMBER_KEY_SEED", "ADMISSION_CONSUMPTION_DIGEST_KEY", "SESSION_TICKET_KEY"]:
        env["REALMMESH_" + name] = secrets.token_hex(32)
    for name, filename in {"TLS_CERTIFICATE_FILE":"edge.pem", "TLS_PRIVATE_KEY_FILE":"edge.key",
                           "SERVICE_CA_FILE":"ca.pem", "SCENE_CERTIFICATE_FILE":"scene.pem",
                           "SCENE_PRIVATE_KEY_FILE":"scene.key", "REALM_CERTIFICATE_FILE":"realm.pem",
                           "REALM_PRIVATE_KEY_FILE":"realm.key"}.items():
        env["REALMMESH_" + name] = str(tls / filename)
    return env


def wait(process, probe, description):
    for _ in range(300):
        if process.poll() is not None:
            raise RuntimeError(f"{description} 已退出 ({process.returncode})")
        try:
            with probe():
                return
        except OSError:
            time.sleep(.1)
    raise RuntimeError(f"{description} 未就绪")


def run(preset):
    build = Path(subprocess.check_output([ROOT / "scripts/build-dir.sh", "--preset", preset], text=True).strip())
    etcd = shutil.which("etcd") or ROOT / ".tools/etcd-v3.6.14/etcd"
    client = build / "tools/browser-demo/realm_browser_client"
    mesh = build / "bin/realm_mesh"
    for path in [client, mesh, etcd]:
        if not os.access(path, os.X_OK):
            raise RuntimeError(f"缺少 {path}；请先完成 README 中的构建和安装步骤。")
    subprocess.run(["node", "--input-type=module", "-e", "await import('ws')"], cwd=DEMO, check=True)
    processes = []
    with tempfile.TemporaryDirectory(prefix="realmmesh-browser-") as temporary:
        directory = Path(temporary)
        config = configure(directory)
        env = service_environment(directory)
        def start(args, **kwargs):
            process = subprocess.Popen(args, cwd=ROOT, **kwargs)
            processes.append(process)
            return process
        try:
            web_port = int(os.environ.get("REALMMESH_DEMO_PORT", "8787"))
            if not 1 <= web_port <= 65535:
                raise RuntimeError("REALMMESH_DEMO_PORT 必须在 1..65535 之间。")
            for port in [12379, 12380, 17443, 17444, 7100, 7200, 8000, web_port, *range(9102, 9107)]:
                with socket.socket() as probe:
                    probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    probe.bind(("127.0.0.1", port))
                    probe.listen()
            peer = "http://127.0.0.1:12380"
            endpoint = "http://127.0.0.1:12379"
            registry = start([etcd, "--data-dir", directory/"etcd", "--listen-client-urls", endpoint,
                              "--advertise-client-urls", endpoint, "--listen-peer-urls", peer,
                              "--initial-advertise-peer-urls", peer, "--initial-cluster", "default="+peer,
                              "--log-level", "error"])
            wait(registry, lambda: urllib.request.urlopen(endpoint+"/health", timeout=.3), "etcd")
            services = start([ROOT/"scripts/with-shared-mongodb.sh", mesh, "--config", config], env=env)
            for port in [17443, 17444, 7100, 7200, 8000]:
                wait(services, lambda: socket.create_connection(("127.0.0.1", port), timeout=.2), f"服务端口 {port}")
            start([ROOT/"scripts/with-shared-mongodb.sh", "node", DEMO/"server.mjs", client, "17443", "17444", "8000"],
                  env=dict(os.environ, REALMMESH_DEMO_SELF_SIGNED="1", REALMMESH_DEMO_REGISTER_DB="realmmesh"))
            while all(process.poll() is None for process in processes):
                time.sleep(.2)
            raise RuntimeError("演示子进程提前退出，请查看终端日志。")
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", default="dev")
    os.umask(0o077)
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    try:
        run(parser.parse_args().preset)
    except KeyboardInterrupt:
        pass
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
