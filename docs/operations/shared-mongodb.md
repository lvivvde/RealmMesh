# 共享临时开发 MongoDB

## 使用边界

macOS 与 Linux 的手动开发联调共享一个 `realmmesh` 数据库。它是可清空重建的临时
开发数据，落盘仅用于正常重启后继续使用，不配置定时备份。自动化测试继续由夹具启动
隔离的临时数据库；运行 CTest 时不使用下面的共享库包装命令。

共享库为 MongoDB 单节点副本集 `rs0`，保留事务与 majority 读写语义，不提供高可用。
服务器只监听 `127.0.0.1:27017`，强制 TLS，使用 SCRAM 账号认证；开发账号只有
`realmmesh` 库的 `readWrite` 权限。远端开发机通过 SSH 隧道接入，MongoDB 端口不向
公网开放。入口保密是附加措施，访问控制由回环监听、SSH、TLS 与数据库认证保证。
SSH 的用户级规则只允许部署用户发起到 `127.0.0.1:27017` 的本地转发，保留全局策略。

## 私有配置与 Agent

`AGENTS.md` 只指向本文与操作命令。实际 VPS 地址和 SSH 私钥由各机器的 SSH 配置管理。
连接材料存放在 `${XDG_CONFIG_HOME:-$HOME/.config}/realmmesh/mongodb/`：

| 文件 | 内容 |
|---|---|
| `env.sh` | 私有 URI、SSH 别名、隧道本地端口 |
| `ca.pem` | 验证 MongoDB 证书所需的 CA 公钥证书 |
| `client.json` | 限定数据库权限的开发账号凭据 |

目录权限为 `0700`，文件为 `0600`。它们位于仓库外，同机的其他聊天和 worktree 可复用。
可以用 `REALMMESH_MONGODB_CONFIG_DIR` 指定另一个私有目录；该目录也应放在仓库外。
提交和推送只包含脚本、环境变量名与通用文档；连接串、IP、密码、私钥与实际证书保留在
机器私有配置中。这些连接脚本不打印 URI，也不把 URI 放进进程参数；诊断时只报告
状态和错误类型，连接材料保持在私有目录内。

## 新 macOS 或远端 Linux 开发机

1. 在仓库外的 SSH 配置中配置 VPS 的 SSH 别名与私钥，通过可信渠道核验主机指纹。
   以下以通用别名 `vps` 为例，仓库不记录它对应的地址。
2. 安装 Python 3；`--check` 还需要 `mongosh`。macOS 可用 Homebrew；Linux 可运行
   `scripts/install-mongodb.sh`，然后用 `REALMMESH_MONGOSH_BINARY` 指定安装后的 shell。
3. 获取私有材料并验证：

```bash
./scripts/configure-shared-mongodb.sh vps
./scripts/with-shared-mongodb.sh --check
```

包装命令按需建立 `127.0.0.1:27018 → SSH → VPS 127.0.0.1:27017` 隧道，复用当前
机器的私有 control socket。若端口冲突，在配置命令前设置
`REALMMESH_MONGODB_LOCAL_PORT` 选择空闲端口。隧道使用 keepalive；SSH 断开后再次运行
包装命令会重建。默认连接使用 `directConnection=true`，避免副本集发现把远端的
`localhost:27017` 当作开发机自己的本地数据库；TLS 仍验证 CA 和回环地址的 SAN。

启动开发服务或其他需要共享库的命令：

```bash
./scripts/with-shared-mongodb.sh ./scripts/dev-services.sh restart
# 单独启动一个服务时也可使用：
./scripts/with-shared-mongodb.sh ./build/dev/bin/realm_mesh --service login_verify --config configs
```

包装命令只注入 MongoDB 连接配置；etcd、服务 TLS、签名材料与其他启动前置条件仍按
原有开发流程准备。URI 中的账号拥有共享库写权限，手动实验须避免清空其他开发任务的
数据。清库操作应由正在负责该共享环境的开发者明确决定。

停止本机隧道：

```bash
ssh -S "${XDG_CONFIG_HOME:-$HOME/.config}/realmmesh/mongodb/ssh-27018.sock" -O exit vps
```

## VPS 本机 Linux

部署脚本已在部署用户的私有配置目录生成本机直连配置：`env.sh` 中 SSH 别名为空，
连接端口为 `27017`。使用同一个 `with-shared-mongodb.sh` 即可，包装命令会跳过 SSH。
新克隆或 worktree 继续读取该用户的私有目录，不需要复制凭据进源码树。

服务由 `realmmesh-mongodb.service` 管理，二进制安装在 `/opt/realmmesh-mongodb/`，
服务端配置、CA 私钥、成员 keyfile 与管理员凭据放在 `/etc/realmmesh-mongodb/`，
数据在 `/var/lib/realmmesh-mongodb/`，日志在 `/var/log/realmmesh-mongodb/`。
开发账号不拥有管理员权限；管理员凭据只在服务器 root 可读文件内。

```bash
sudo systemctl status realmmesh-mongodb
sudo systemctl restart realmmesh-mongodb
sudo systemctl show realmmesh-mongodb -p MemoryCurrent -p MemoryHigh -p MemoryMax
```

这套约 1 GiB 内存的小机器配置使用 0.256 GB WiredTiger 缓存、128 MB oplog、
最多 64 条入站连接、480 MiB 内存软阈值和 600 MiB 服务内存硬上限。
缓存上限不等于总内存上限；触达硬上限时数据库可能被终止，不能用于大量压测。
日志通过 logrotate 保留三份轮转文件，避免小磁盘长期累积日志。

## 配置入口与重新部署

`configs/common/player_data.lua` 的 `uri_environment` 指定
`REALMMESH_MONGODB_URI`。该变量有值时覆盖默认本机 URI；变量未设置时保留本机开发
默认值；显式设置为空或既无变量又无默认 URI 时，配置加载失败。解析错误不输出 URI。

`scripts/provision-shared-mongodb.sh` 与 `scripts/install-mongodb.sh` 一起上传后，通过
sudo 运行前者即可部署。脚本使用固定版本及 SHA256 校验，创建独立服务账号、证书、
随机凭据和数据库限定权限的客户端账号；默认客户端用户取 `SUDO_USER`，也可显式指定
`REALMMESH_MONGODB_CLIENT_USER`。正在运行的实例会被保留，脚本不会自动清库。
服务器证书有有效期，需在到期前重新签发；CA 变更后各开发机重新获取私有材料。

## 验收记录（2026-10-01）

macOS 经 SSH 隧道与 VPS 本机 Linux 都通过 TLS、账号认证、`rs0` primary 与 majority
事务检查；匿名访问、错误密码、跨数据库读取和不可信 CA 被拒绝。真实 C++
Login Verifier 使用私有 URI 完成共享开发账号认证。配置加载相关 17 项测试通过。
部署后服务内存约 130 MiB，机器仍有约 500 MiB 可用；这只是轻量联调时的实测，
不代表负载容量验收。待提交文件核验未包含实际 VPS 地址、数据库密码或私钥。
