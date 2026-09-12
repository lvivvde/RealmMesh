# 调研：身份 Token（JWT/EdDSA）细节与边缘验签契约

- 日期：2026-09-12
- 票据：[lvivvde/RealmMesh#25](https://github.com/lvivvde/RealmMesh/issues/25)（地图 [#23](https://github.com/lvivvde/RealmMesh/issues/23)）
- 状态：已定稿（推荐结论见 [TL;DR](#tldr)）
- 方法：一手来源核查——本仓库源码/CMake/ADR 逐文件核实；RFC 7515/7517/7519/8037/8725 逐条款核对；libsodium 官方文档与 vendored 1.0.22 头文件逐函数核对；候选库官方 README 与算法表逐条核对；Cloudflare/Akamai/Fastly 官方产品文档（2026-09-12 快照）逐页核对。所有引用见文末。

## TL;DR

**推荐自研：在 `game/common/` 新增最小 EdDSA-only Compact JWS 编解码（有界 codec，与 `LengthFieldCodec` 同型），签名原语用现有 vendored libsodium 的 `crypto_sign_detached`（纯 Ed25519，64 字节裸 R‖S 签名，与 JOSE EdDSA 逐字节兼容）；不引第三方 JWT 库，零新依赖。** claims 最小集 = header `alg`/`typ`/`kid` + payload `iss`/`sub`/`aud`/`exp`/`iat`/`jti`/`purpose`；公钥经 Login Verifier 的 JWKS 端点分发（RFC 7517），`kid` 双键重叠轮换，内部消费方（排队调度服/网关）同一 JWKS 拉取、dev 拓扑可配置固化。TTL：身份 Token 30 min、排队号牌 30 min、EnterGame 票据 5 min，验签时钟容差统一 60 s。边缘验签可行但**强依赖供应商且能力分级**：免代码产品面（Cloudflare API Shield、Akamai API Gateway、Fastly VCL）均不支持 EdDSA；EdDSA 边缘验签只能落在可编程计算面——Cloudflare Workers 一等支持（WebCrypto Ed25519），Fastly Compute 可代码实现，Akamai 可编程面当前无 Ed25519、不可落地。

关键理由：

1. **RFC 8037 把 EdDSA 收敛为一个小原语，libsodium 直配。** JOSE 的 EdDSA 就是「对 JWS Signing Input 做纯 Ed25519 签名」（RFC 8037 §3.1），无预哈希、无 DER；libsodium 单段 `crypto_sign_detached` 恰是这个原语（64 字节裸签名 = `crypto_sign_BYTES 64U`，vendored 1.0.22 头文件核实）。多段 API 才是 Ed25519ph（"intentionally not equivalent to Ed25519(SHA512(m))"），JWT 不用它。自签的密码学风险面因此只剩 base64url 与固定 schema JSON 两个 codec，且 RFC 8037 附录 A 提供签发/验签全套固定测试向量可直接入 GTest。
2. **引库的依赖面不合算。** jwt-cpp 的 Ed25519 路径要求 libcrypto（OpenSSL/LibreSSL/wolfSSL），libjwt 要求 OpenSSL ≥ 3.0.0 或 GnuTLS ≥ 3.8.8 后端再加 Jansson/json-c——都会给 `game/common` 拖第二条加密栈（OpenSSL 目前只是 `realm_network` 的依赖）。而 RFC 8725 §3.1 本来就要求把算法白名单收窄（"each key MUST be used with exactly one algorithm"），通用库的多算法面对我们是负资产。
3. **票面「与现有 SessionTicket 资产同源」必须修正一处事实：现有 SessionTicket 是对称 HMAC-SHA256（`crypto_auth_hmacsha256`），不是 Ed25519。** 「同源」只能指 libsodium 这个库。正确分工：身份 Token 用 Ed25519（私钥只在签发方）；号牌与 EnterGame 票据留在 HMAC codec（签发方=验证方，对称成立）——即库同源、原语分家。
4. **边缘验签的真实生态已核实为「能力分级」。** Cloudflare：Workers WebCrypto 原生支持 Ed25519 verify+importKey，但免代码的 API Shield JWT 校验算法面只有 RS/PS/ES/HS；Akamai：API Gateway 免代码验签仅 RSA，EdgeWorkers 的 crypto 模块无 Ed25519；Fastly：VCL 原生仅 ES256，Compute 可代码实现。姊妹调研（#24 问题 4）的「边缘验签→回源明文」契约选项只在具备 EdDSA 能力的边缘拓扑成立，契约文本必须写成能力分级。

---

## 背景与仓库事实（已核实）

- **SessionTicket 资产**（`game/common/src/session_ticket.cpp`）：`crypto_auth_hmacsha256` / `_verify` 对称 HMAC-SHA256；32 字节共享密钥经 `REALMMESH_SESSION_TICKET_KEY` 环境变量与 `parse_ticket_key_hex` 注入（`framework/service_host/src/service_frame.cpp`）。二进制定长格式：version(1B) + purpose(1B) + ticket_id(16B 随机) + [correlation_id(16B)] + account_id(8B) + realm_id(4B) + character_id(8B) + expiry(8B 毫秒) + tag(32B)；TTL 由调用方传入；`TicketReplayGuard` 进程内一次性消费，`SessionTickets::redeem` 原子完成「验证+消费」。`TicketPurpose` 仅 `Login`(1)/`EnterGame`(2)。
- **libsodium vendored**：`third_party/sodium/CMakeLists.txt` ExternalProject 固定 1.0.22-RELEASE 静态库，`realm_game_common` PUBLIC 链接 `Sodium::Sodium`——身份 Token 编解码落同一目标，零 CMake 改动。
- **OpenSSL 3.0 目前只是 `realm_network` 的 PUBLIC 依赖**（#24 姊妹调研核实），`game/common` 无 OpenSSL。
- **CONTEXT.md「Access」**：Login Verifier 签发身份 Token；边缘、排队调度服、网关各自本地验签、不引入共享状态；Queue Number 由调度服签发，服务端唯一权威状态是已放行号。
- **地图 #23 定案**：1M/10min 洪峰（≈1700/s 放行）、轮询 ≥2s 起步自适应；号牌为签名 Token；干净切换、无旧客户端兼容；边缘（CDN/WAF/DDoS）只定契约、不在本仓库实现。
- **architecture.md**：EnterGame 票据在 Gateway 兑换触发 pending→established，重放/无效按鉴权失败断开；当前无恢复 token。

## 问题 1：claims 最小集与各消费方视图

**header（profile 强制）：**

| 参数 | 值 | 依据 |
|---|---|---|
| `alg` | `EdDSA`（唯一白名单值） | RFC 7515 §4.1.1 "MUST be present"；RFC 8037 §3.1；RFC 8725 §3.1 白名单与 key 绑定 |
| `typ` | `JWT` | RFC 7519 §5.1 "RECOMMENDED that its value be 'JWT'" |
| `kid` | RFC 7638 thumbprint（base64url） | RFC 7515 §4.1.4 定义为 hint（spec 层 optional）；本 profile 设为强制，理由：JWKS 多键并存时无选择子即无法验签；RFC 7517 §4.5 明言 kid "to choose among a set of keys within a JWK Set during key rollover"；Cloudflare API Shield 亦要求 JWK 带 `kid` 且 JWT header 匹配（生态惯例佐证） |

**payload（最小集，全部一条 token 携带）：**

| claim | 值 | 消费方 | 依据/理由 |
|---|---|---|---|
| `iss` | Login Verifier 外部 base URI（如 `https://login.realmmesh.example`） | 全部验 | RFC 7519 §4.1.1；含 `:` 必须是 URI |
| `sub` | account_id 的**十进制字符串** | 排队调度服（排队实体）、网关（绑定票据） | RFC 7519 §4.1.2 "MUST be a string containing a StringOrURI value"；仓库内硬理由：`account_id` 是 uint64，数值超出 2^53 会被 JS 消费方（Workers/EdgeWorkers 的 JSON number）失真，字符串形式规避 |
| `aud` | 单值 `realmmesh-access` | 全部验 | RFC 7519 §4.1.3（单 aud 可为 string，不匹配即 MUST reject）；RFC 8725 §3.9 "the relying party or application MUST validate the audience value" |
| `exp` | NumericDate 整数秒 | 全部验 | RFC 7519 §4.1.4 / §2 NumericDate（1970 起秒，忽略闰秒） |
| `iat` | NumericDate 整数秒 | 诊断/审计 | RFC 7519 §4.1.6；成本 4 字节 |
| `jti` | 16 字节随机 hex | 网关（单次入场消费） | RFC 7519 §4.1.7 "can be used to prevent the JWT from being replayed"；随机性满足跨 issuer 唯一要求 |
| `purpose` | `access`（私有 claim，值域预留） | 排队调度服、网关验 | 对应 SessionTicket 的 purpose 语义，防 token 类型混用 |

**不放入**：`realm_id`/`character_id`（选角/Realm 链路移到网关准入之后，由网关会话与 EnterGame 票据承载）；`nbf`（单 issuer 单时钟源签发，`exp` 足够，少一处时钟误配面）；封禁/白名单等状态类 claim（签发后可变，时效性矛盾，由签发时点检查与网关准入时点校验兜底）。

**各消费方视图（一张 token 三方验）：**

- **边缘（可编程计算面）**：验 `alg` 白名单 + `kid` 选键 + 签名 + `exp`/`aud`；不解释 `sub` 语义。验签通过后可把已验证声明回写头（如 `x-rm-account`）供明文回源拓扑使用。
- **排队调度服**：验同上 + `purpose`；读 `sub`（排队与幂等键）、`jti`（审计）。
- **网关**：验同上 + `purpose`；读 `sub` 与 EnterGame 票据 `account_id` 做绑定校验；`jti` 进程内消费表做单次入场（防同 token 双开）——与 `TicketReplayGuard` 同型的进程内有界状态，不跨进程共享，不违反零共享状态前提。

## 问题 2：实现路径——libsodium 自签 vs 引库

| 路线 | 许可 | EdDSA/Ed25519 | 依赖面 | 结论 |
|---|---|---|---|---|
| **自研 EdDSA-only Compact JWS（libsodium `crypto_sign_detached`）** | 无新依赖 | 纯 Ed25519 detached，64B 裸签名，与 RFC 8037 逐字节兼容 | 零（`Sodium::Sodium` 已在 `realm_game_common`） | **推荐** |
| jwt-cpp | MIT | 算法表含 Ed25519/Ed448 | header-only；EdDSA 路径强制 libcrypto（OpenSSL/LibreSSL/wolfSSL）+ JSON traits | 否——给 game/common 引 OpenSSL |
| libjwt | MPL-2.0 | Ed25519 ✅（OpenSSL ≥ 3.0.0 / GnuTLS ≥ 3.8.8 后端） | C 库 + Jansson/json-c + 上述加密后端 | 否——两条新依赖 |
| latchset/jose | Apache-2.0 | **README 算法表无 EdDSA**，源码树 ed25519 检索 0 命中 | OpenSSL + jansson | 否——不支持 |

**自研要点（全部一手核实）：**

- 签名输入 = `ASCII(BASE64URL(UTF8(header)) || '.' || BASE64URL(payload))`（RFC 7515 §2）；compact 序列化三段两点（§7.1）。
- libsodium：`crypto_sign_keypair` / `crypto_sign_seed_keypair`（32B seed，存配置同 `REALMMESH_SESSION_TICKET_KEY` 模式）；`crypto_sign_detached` / `crypto_sign_verify_detached`；默认确定性签名；**不要用多段 API**（那是 Ed25519ph，JOSE 不用）；`crypto_sign_ed25519_sk_to_pk` 可从 sk 提取发布公钥。
- 需要自写的只有三件：base64url（无填充，编码+解码）、固定 schema 的 JSON 最小序列化/受限解析（claims 集闭合，与 `LengthFieldCodec` 同型的有界 codec）、JWK/JWKS 导出（`kty=OKP`、`crv=Ed25519`、`x`=32B 公钥 base64url，RFC 8037 §2）。
- 验收锚点：RFC 8037 附录 A.4（完整 compact JWS 签发向量）/A.5（验证向量）入 GTest 固定向量；负例覆盖 `alg`≠EdDSA 拒收、未知 `crit` 拒收（RFC 7515 §4.1.11）、kid 缺失、签名截断、非规范 base64url（RFC 7519 §7.2 "no line breaks, whitespace, or extra characters allowed"）。
- 位置：`game/common/`（与 session_ticket 同目标同层，均为「业务凭据编解码」职责）；测试 `tests/cpp/game/common/`，标签 `unit`。

## 问题 3：签名密钥分发与轮换

前提：公钥不是秘密，分发只要求**完整性**（TLS + 主体校验），不要求保密；私钥只在 Login Verifier，永不进边缘。

- **分发**：Login Verifier 暴露 JWKS 端点（`GET /.well-known/jwks.json`，RFC 7517 §5 "The JSON object MUST have a 'keys' member"），响应带 `Cache-Control` 控制消费方缓存 TTL。三个消费方同一 JWKS：边缘计算面 fetch+缓存；排队调度服/网关生产拓扑周期拉取、dev/离线拓扑配置固化公钥（32B pk hex，同现有 key 配置模式）。
- **kid**：RFC 7638 thumbprint（RFC 8037 §2 给出 `crv`/`kty`/`x` 字典序 canonicalization，附录 A.3 有完整算例）——确定性生成，无注册表。
- **轮换**：生成新键 → JWKS 同时列新旧两把（`kid` 区分）→ 签发切新键 → 旧键保留至「最后一张旧签 token 的 `exp` + JWKS 缓存 TTL」后摘除。重叠窗 = 身份 Token TTL（30 min）+ JWKS 缓存 TTL（≥1 h），即至少数小时。
- **生态佐证**：Cloudflare API Shield 的 JWKS 是 inline 配置、`PUT` 全量替换/`PATCH` 按 `alg`+`kid` 匹配保留（其文档还建议用 Worker 保持 JWKS 新鲜）——说明「kid 多键并存 + 消费方缓存」是边缘生态的标准轮换形态；自建 Workers 路径直接 fetch JWKS + Cache API 即可。

## 问题 4：TTL 矩阵

| 凭据 | TTL | 为什么 |
|---|---|---|
| 身份 Token | **30 min** | 须覆盖「登录→排队→放行→进网关」全链：最坏排队 ≈ 10 min 放行窗口，30 min = 3× 余量。v1 无 refresh（Login Verifier 无状态、无存储层）；入场后 token 无进一步用途（无恢复 token，断线重连=重新登录重新排队），泄露敞口被 jti 一次性入场收敛。诚实风险：若放行延后过 TTL，客户端需重新登录（重新拿 token+号牌、重新排队）——v1 取舍，规格定稿时复核。 |
| 排队号牌 | **30 min**（与身份 Token 同窗） | 号牌签发方=验证方（调度服自签自验，唯一状态「已放行号」在本服务），HMAC 成立。断线找回凭号牌重查位次；TTL 不得晚于身份 Token（放行入场时两者都需有效）。 |
| EnterGame 票据 | **5 min** | 放行时刻签发、随即建连入场，覆盖 QUIC/TLS 竞速握手窗与少量重试；兑换点单次消费在 Gateway，TTL 短=重放窗短。 |
| 验签时钟容差 | **60 s**（统一） | RFC 7519 允许 "some small leeway, usually no more than a few minutes"；取 60 s 与边缘产品量级一致（Cloudflare API Shield 固定 60 s，Akamai 默认 5 s、可配 0–60 s）。 |

号牌 TTL 备选：由调度服按实时放行速率动态写 `exp`（放行时刻+宽限）——更省但引入速率估算 fragility，v1 不取，规格可复核。

## 问题 5：边缘验签契约可行性（Cloudflare / Akamai / Fastly，2026-09-12 快照）

| 平台 | 免代码产品面 | 可编程计算面 | EdDSA 边缘验签结论 |
|---|---|---|---|
| Cloudflare | API Shield JWT validation：算法面 = RS256/384/512、PS256/384/512、ES256/ES384、HS256/384/512，**无 EdDSA**；token 源仅 header/cookie（query/POST 不支持）；JWKS inline 配置；WAF 规则语言无任何非对称验签函数（`is_jwt_valid` 亦绑 API Shield 配置） | Workers：WebCrypto **原生支持 Ed25519 verify + importKey**（Secure Curves API）；官方兼容 jose（RFC 8037 在列，声明支持 Cloudflare Workers） | **可行（Workers）** |
| Akamai | API Gateway JWT validation：**仅 RSA**（"supports the use of RSA private/public key pairs"）；键=手动上传主备两把或 jku+allowed hosts；验 exp/nbf/iss/sub/aud，无 jti | EdgeWorkers：`jwt` 模块（JWTValidator + CryptoKey[]，WebCrypto 语义）但其 `crypto` 模块 sign/verify 仅 HMAC/RSA-PSS/RSASSA-PKCS1-v1_5/ECDSA（示例仅 P-256），importKey 表**无 Ed25519** | **当前不可落地** |
| Fastly | VCL：原生仅 `digest.ecdsa_verify`（P-256/ES256，官方文档明确用于验 JWT ES256），无 Ed25519 | Compute（Rust/JS）：官方 starter kit `compute-rust-auth` 自带 JWT 验证骨架，Rust 配 `ed25519-dalek` / JS 配 `@noble/curves` 即验 EdDSA | **可行（Compute 代码）** |

**Workers 形态样例（规格级示意，非产品代码）：**

```js
// Cloudflare Worker，route: api.example.com/queue/* ；token 源: Authorization: Bearer
const jwks = await fetchJwksCached(env, ctx);            // JWKS URL + Cache API，Cache-Control 控制缓存
const [h, p, s] = token.split(".");
const jwk = jwks.keys.find(k => k.kty === "OKP" && k.alg === "EdDSA"
                             && k.kid === b64urlJson(h).kid);
const key = await crypto.subtle.importKey("jwk", jwk, { name: "Ed25519" }, false, ["verify"]);
const ok = await crypto.subtle.verify({ name: "Ed25519" }, key,
    b64urlDecode(s), new TextEncoder().encode(`${h}.${p}`));
// ok 之后仍须由调用方校验 exp/aud/purpose（claims 字段），再转发或回写 x-rm-account
```

**契约条款要点（与 #24 问题 4 衔接）：**

1. **本地验签是硬性义务，边缘验签是能力分级优化。** 排队调度服与网关本地验签不可省略（已定前提）；边缘验签挡掉无效流量，不是信任锚的唯一持有者——origin 侧（明文回源拓扑中的监听服务）仍保留本地验签作纵深。
2. 「边缘验签 → 回源明文」拓扑仅在边缘具备 EdDSA 能力时成立（Cloudflare Workers、Fastly Compute）；Akamai 拓扑回退「边缘仅 TLS 终结与 L3-L7 防护、回源 HTTPS、回源验签」——姊妹调研已定「HTTPS 监听是本体，明文回源是拓扑契约选项」，此处给出该选项的供应商约束。
3. token 位置契约：`Authorization: Bearer`（header）；各家产品面对 cookie/query 的支持不一（API Shield 支持 cookie 不支持 query），固定 header 是交集。
4. 边缘拒绝行为非我方控制（如 API Shield 默认 403），客户端契约不得依赖边缘错误体的形态。
5. 排队轮询接口的边缘缓存策略需显式固定（`Cache-Control`/`Vary`）；token 在 Authorization 头，缓存键不得引入跨账号串号维度。
6. 中国区供应商未在本票核查范围（地图 Out of scope：CDN/WAF 供应商选型）；契约按上述标准 JWT 形态书写，供应商落地属选型票。

## 问题 6：与现有 libsodium SessionTicket 的边界

| 项 | 处置 | 理由 |
|---|---|---|
| HMAC codec（`crypto_auth_hmacsha256` 32B key，签发验证同址） | **保留** | 签发方=验证方的场景（调度服号牌、EnterGame 票据）对称成立且成本最低 |
| `TicketPurpose::EnterGame` | **保留**，语义不变（Gateway 兑换、pending→established、单次消费） | 干净切换后流程归位，claims（account_id/realm_id/character_id）不动 |
| 排队号牌 | **新增 purpose**（如 `QueueNumber`(3)），复用同一 HMAC codec 与 `SessionTickets` 类 | 号牌签发方=验证方=调度服；与「号牌为签名 Token」定案一致；CONTEXT.md 术语上仍是 Queue Number，不复用 Session Ticket 名 |
| `TicketPurpose::Login` | **退役** | 旧链路 client→Login→Realm 直连模型随「Login Verifier 取代旧 Login 对外职责、选角/Realm 移到网关准入之后」消失，身份 Token + 网关会话取代其位置；干净切换无兼容负担 |
| 32B 对称密钥跨服务共享 | **不再出现** | 身份 Token 私钥只在 Login Verifier；对称 key 只留在签发=验证同址场景 |

## 与已定前提的冲突清单（显式指出）

1. **「libsodium Ed25519 自签 JWT（与现有 SessionTicket 资产同源）」**：SessionTicket 现用对称 HMAC-SHA256，不是 Ed25519；同源只能指 libsodium 库本身。结论不受影响（仍推荐 libsodium 自签），事实表述需按本调研修正。
2. **「边缘可验签」**：技术上成立（Cloudflare Workers / Fastly Compute），但免代码产品面三家均无 EdDSA、Akamai 全线当前无 Ed25519——边缘验签契约必须写成**能力分级**，明文回源选项限定在具备 EdDSA 能力的边缘拓扑。地图定案本身不被推翻，约束被细化。
3. 无其他冲突；本票只出规格与契约文本，不出产品代码（前提保持）。

## 对规格的影响面（后续票引用）

- API 契约票：身份 Token profile（header/payload 表）、token 位置、错误码与重登触发。
- 排队语义票：号牌 purpose 扩展、TTL 30 min、断线找回窗口。
- 网关准入票：jti 消费表 + EnterGame 票据 `account_id`↔`sub` 绑定校验。
- 边缘契约文本：能力分级表（本文问题 5）+ 转发声明头约定 + 拒绝行为非保证条款。
- 退役清单票：`TicketPurpose::Login` 移除时序。

## 参考链接

- 仓库事实（逐文件核实）：`game/common/src/session_ticket.cpp`、`game/common/include/realmmesh/game/common/session_ticket.hpp`、`game/common/CMakeLists.txt`、`third_party/sodium/CMakeLists.txt`、`framework/service_host/src/service_frame.cpp`（`REALMMESH_SESSION_TICKET_KEY`）、`CONTEXT.md`「Access」「Messaging」、`docs/architecture.md`、`docs/adr/0001`、`docs/adr/0003`、`docs/research/2026-09-12-https-stack.md`
- RFC 7515（JWS；§2 Signing Input、§4.1.1 alg、§4.1.4 kid、§4.1.11 crit、§7.1 compact）：https://www.rfc-editor.org/rfc/rfc7515.html
- RFC 7519（JWT；§4.1 注册 claims、§2 NumericDate、§5.1 typ、§7.2 验证）：https://www.rfc-editor.org/rfc/rfc7519.html
- RFC 8037（CFRG Ed25519/Ed448 in JOSE；§2 OKP/x/thumbprint、§3.1 纯 EdDSA、附录 A 测试向量）：https://www.rfc-editor.org/rfc/rfc8037.html
- RFC 7517（JWK/JWK Set；§4.5 kid rollover、§5 keys 数组）：https://www.rfc-editor.org/rfc/rfc7517.html
- RFC 8725（JWT BCP；§3.1 算法验证、§3.9 aud 校验）：https://www.rfc-editor.org/rfc/rfc8725.html
- libsodium 官方文档 Public-key Signatures（detached、Ed25519ph 多段 API 警告、确定性签名）：https://doc.libsodium.org/public-key_cryptography/public-key_signatures
- libsodium 1.0.22-RELEASE `crypto_sign_ed25519.h`（64U/32U/64U 常量）：https://github.com/jedisct1/libsodium/blob/1.0.22-RELEASE/src/libsodium/include/sodium/crypto_sign_ed25519.h
- jwt-cpp（README：header-only、EdDSA 表含 Ed25519/Ed448、libcrypto 依赖）：https://github.com/Thalhammer/jwt-cpp
- libjwt（README：Ed25519 OpenSSL ≥ 3.0.0 / GnuTLS ≥ 3.8.8、Jansson/json-c、MPL-2.0）：https://github.com/benmcollins/libjwt
- latchset/jose（README 算法表无 EdDSA）：https://github.com/latchset/jose
- Cloudflare Workers WebCrypto（Ed25519 verify/importKey，Secure Curves；NODE-ED25519 限制）：https://developers.cloudflare.com/workers/runtime-apis/web-crypto/
- Cloudflare API Shield JWT validation（配置模型、token 源限制、inline JWKS、kid+alg 匹配）：https://developers.cloudflare.com/api-shield/security/jwt-validation/ ；算法面（RS/PS/ES/HS，无 EdDSA）与轮换 API：https://developers.cloudflare.com/api-shield/security/jwt-validation/api/
- Cloudflare Ruleset Engine 函数参考（规则语言无 JWT 非对称验签函数）：https://developers.cloudflare.com/ruleset-engine/rules-language/functions/
- panva/jose（支持 Cloudflare Workers、RFC 8037 在列、基于 WebCrypto）：https://github.com/panva/jose
- Akamai API Gateway JWT validation（仅 RSA、jku/allowed hosts、claims 面、clock skew 0–60s）：https://techdocs.akamai.com/api-definitions/docs/json-web-token-jwt-val
- Akamai EdgeWorkers crypto 模块（sign/verify 仅 HMAC/RSA/ECDSA，无 Ed25519）：https://techdocs.akamai.com/edgeworkers/docs/crypto ；jwt 模块（JWTValidator/CryptoKey）：https://techdocs.akamai.com/edgeworkers/docs/jwt
- Fastly VCL `digest.ecdsa_verify`（P-256/ES256，JWT 验签用途）：https://www.fastly.com/documentation/reference/vcl/functions/cryptographic/digest-ecdsa-verify/
- Fastly 官方 JWT 边缘认证示例：https://www.fastly.com/documentation/solutions/examples/json-web-tokens/ ；starter kit（JWT 验证骨架）：https://github.com/fastly/compute-rust-auth 、https://github.com/fastly/compute-js-auth
