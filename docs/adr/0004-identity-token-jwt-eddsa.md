# 身份 Token 采用标准 JWT（EdDSA）并由边缘验签放行

---
status: accepted
---

登录健全服签发的身份 Token 必须能被边缘（CDN/WAF）与内部服务（排队调度服、网关）各自本地验签。选择标准 JWT（EdDSA/Ed25519）：边缘生态（Cloudflare API Shield、Akamai API Gateway、Fastly Compute、阿里云 API Gateway）有原生化验签能力，自研格式做不到这一点；Ed25519 与现有 libsodium 资产同源。内部实现走自研 Compact JWS（libsodium + 极小 JSON，零新依赖），不引入 JWT 库——内部消费方只验自家 token，JWKS 是我们生成给边缘的，不需要 JWK 解析能力。

## Considered Options

- **libjwt / jwt-cpp**：被否。EdDSA 路径强制 OpenSSL/GnuTLS 后端（libjwt 另为 LGPL-3.0），而 JWK/JWKS 解析能力内部用不上。
- **对称 HMAC（含沿用 SessionTicketCodec 单键方案）**：被否。边缘验签要求把共享密钥放进边缘供应商配置，泄漏面不可控且无法独立轮换——非对称是边缘验签的硬前提。
- **自研非标签名 Token**：被否。边缘产品无内置验签能力，与"边缘可验签放行"的契约定案冲突。

## Consequences

- 签名密钥分发与轮换走 `kid` + JWKS（24h 重叠窗双键）；私钥只在健全服（env 注入）。
- 封禁/白名单状态不入 token，时效性由签发时检查与网关拉取二次校验兜底。
- 边缘 block 行为不保证返回我方 JSON 错误体（如 Cloudflare 默认 403 HTML），客户端契约须容忍。
