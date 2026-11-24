import http from "node:http";
import { readFileSync } from "node:fs";
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { WebSocketServer } from "ws";

const [binary, ...ports] = process.argv.slice(2);
if (
  !binary ||
  ports.length !== 3 ||
  ports.some((p) => !/^\d+$/.test(p) || +p < 1 || +p > 65535)
)
  throw Error(
    "用法：node server.mjs <realm_browser_client> <登录端口> <排队端口> <Gateway端口>",
  );
const port = Number(process.env.REALMMESH_DEMO_PORT ?? 8787);
const host = "127.0.0.1:" + port;
const page = readFileSync(new URL("./index.html", import.meta.url));
const assets = new Map(
  ["/world.js", "/three.module.js", "/three.core.js"].map((path) => [
    path,
    readFileSync(
      new URL(
        path === "/world.js"
          ? "./world.js"
          : "./node_modules/three/build" + path,
        import.meta.url,
      ),
    ),
  ]),
);
const server = http.createServer((req, res) => {
  if (req.headers.host !== host || req.method !== "GET")
    return res.writeHead(404).end();
  if (assets.has(req.url))
    return res
      .writeHead(200, { "Content-Type": "text/javascript" })
      .end(assets.get(req.url));
  if (req.url !== "/") return res.writeHead(404).end();
  res
    .writeHead(200, {
      "Content-Type": "text/html; charset=utf-8",
      "Cache-Control": "no-store",
    })
    .end(page);
});
const sockets = new WebSocketServer({ noServer: true, maxPayload: 2048 });
server.on("upgrade", (req, socket, head) => {
  if (
    req.url !== "/play" ||
    req.headers.host !== host ||
    req.headers.origin !== "http://" + host ||
    sockets.clients.size >= 8
  )
    return socket.destroy();
  sockets.handleUpgrade(req, socket, head, (ws) =>
    sockets.emit("connection", ws),
  );
});
sockets.on("connection", (ws) => {
  const child = spawn(binary, ports, { stdio: ["pipe", "pipe", "inherit"] });
  let pending = false;
  ws.on("message", (data, binaryMessage) => {
    if (pending || binaryMessage) return ws.close(1008);
    try {
      child.stdin.write(JSON.stringify(JSON.parse(data.toString())) + "\n");
      pending = true;
    } catch {
      ws.close(1007);
    }
  });
  createInterface({ input: child.stdout }).on("line", (line) => {
    if (!pending || ws.readyState !== 1) return;
    const message = JSON.parse(line);
    if (!Object.hasOwn(message, "position")) pending = false;
    ws.send(line);
  });
  child.on("error", () => ws.close(1011));
  child.on("exit", () => ws.close());
  child.stdin.on("error", () => ws.close());
  ws.on("error", () => child.kill());
  ws.on("close", () => child.kill());
});
server.listen(port, "127.0.0.1", () =>
  console.log("RealmMesh: http://" + host),
);
