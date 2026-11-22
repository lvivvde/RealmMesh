# 本机双人网页演示

先按[共享开发 MongoDB](shared-mongodb.md)配置本机并确认连接正常。开发账号来自
`configs/common/accounts.lua` 的首次导入；已有数据库以库内账号与口令为准。
演示会正常保存角色和移动落点，不清空数据库。回归测试使用隔离夹具，不运行本启动器。

准备 CMake 开发环境、Python 3、OpenSSL（支持 `req -addext`）、Node.js 22 或更新版本及 npm。
在仓库根目录执行：

```bash
./scripts/install-etcd.sh
cmake --preset dev
cmake --build --preset dev --target realm_mesh realm_browser_client
npm ci --prefix tools/browser-demo
./scripts/run-browser-demo.py --preset dev
```

脚本在临时目录生成专用签名材料、接入证书、Realm/Scene mTLS 身份与配置副本；
它运行独立 etcd，经共享 MongoDB 包装器启动五个服务，并在服务开始监听后启动网页。
浏览器、Gateway 与 Realm 的接入都只监听回环地址。若已有使用相同端口的开发进程，
先停止它再启动演示。按 Ctrl-C 停止本次启动的进程并回收临时目录。

打开终端输出的本机网址（默认 `http://127.0.0.1:8787/`），在两个窗口用不同账号登录。
也可以点「注册账号」创建新的开发账号，再用该账号登录和创建角色。注册会持久写入共享开发数据库，请使用专用演示口令。注册仅在本机启动器配置后可用，不改变游戏服务的登录协议。
进场后的「地图」显示附近角色与自身确认坐标；「物品栏」目前仅展示空背包。
默认导入的 `player` 和 `pinned` 都可用于演示，口令见开发账号配置或询问数据维护者。
每个窗口选择角色或创建一个新角色，确认两个窗口都显示两人。先在甲窗口向右拖动摇杆，
观察乙窗口中的甲；再在乙窗口向下拖动并松手，观察甲窗口中的乙停在相应位置。
首次出生点在坐标下界，向左或向上可能被 Scene 拒绝，先向右或向下移动。

网页只连接本机兼容进程。演示使用临时自签接入证书；Scene 内部连接仍校验独立 CA
和服务身份。不要把本机试玩端口反向代理至公网。客户端接口与验收范围见
[首版规格](../specs/2026-11-18-browser-mmo-demo.md)。
