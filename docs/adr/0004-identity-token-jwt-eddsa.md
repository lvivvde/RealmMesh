# 身份 Token 采用标准 JWT（EdDSA）并由边缘验签放行

---
status: accepted
---

登录健全服签发的身份 Token 必须能被边缘（CDN/WAF）与内部服务（排队调度服、网关）各自本地验签。选择标准 JWT（EdDSA/Ed25519）：它是唯一能被边缘生态验签的标准格式，自研格式做不到这一点；Ed25519 与现有 libsodium 资产同源。边缘验签能力经逐家核实为**分级**：免代码产品面（Cloudflare API Shield、Akamai API Gateway、Fastly VCL）均不支持 EdDSA，EdDSA 边缘验签只能落在可编程计算面（Cloudflare Workers 原生 WebCrypto、Fastly Compute 可代码实现），Akamai 当前全线无 Ed25519、拓扑须回退「边缘 TLS 终结 + 回源 HTTPS + 回源验签」。内部实现走自研 Compact JWS（libsodium + 极小 JSON，零新依赖），不引入 JWT 库——内部消费方只验自家 token，JWKS 是我们生成给边缘的，不需要 JWK 解析能力。

## Considered Options

- **libjwt / jwt-cpp**：被否。EdDSA 路径强制 OpenSSL/GnuTLS 后端（libjwt 另为 LGPL-3.0），而 JWK/JWKS 解析能力内部用不上。
- **对称 HMAC（含沿用 SessionTicketCodec 单键方案）**：被否。边缘验签要求把共享密钥放进边缘供应商配置，泄漏面不可控且无法独立轮换——非对称是边缘验签的硬前提。
- **自研非标签名 Token**：被否。没有任何边缘产品能理解自研格式，连可编程面也要为它单独实现解析，与"边缘可验签放行"的契约定案冲突。

## Consequences

- 本地验签是硬性义务、边缘验签是能力分级优化：健全服/排队服/网关的本地验签不可省略，边缘验签只是把无效流量挡在边缘的优化；无 EdDSA 能力的边缘（如 Akamai）下系统整体降级为回源验签，功能不受损。
- 「边缘验签 → 回源明文」契约选项（ADR-0007）仅在具备 EdDSA 能力的可编程边缘（Workers/Compute 类）成立。
- 签名密钥分发与轮换走 `kid` + JWKS（24h 重叠窗双键）；私钥只在健全服（env 注入）。
- 封禁/白名单状态不入 token，时效性由签发时检查与网关拉取二次校验兜底。
- 边缘 block 行为不保证返回我方 JSON 错误体（如 Cloudflare 默认 403 HTML），客户端契约须容忍。
