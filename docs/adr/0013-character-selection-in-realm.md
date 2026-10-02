# 选角移入 Realm Session,EnterRealm 票据只绑定账号与 Realm

---
status: accepted
---

此前 Gateway 在 Fetching 阶段读取账号的选定角色,无选定角色即以 1007 拒绝,并把 (account, character, realm) 写入 EnterRealm 票据。这让没有角色的新账号永远进不了 Realm,也就无从创建角色。现改为:EnterRealm 票据只绑定 (account, realm);Realm Session 入场后先处于选角阶段,在 Realm 内列出、创建、选中角色后才接受游戏动作。Realm 成为角色的唯一业务入口,Selected Character 降为「上次所选」的客户端默认提示。选角发生在直连之后,不改变 ADR-0005「网关先限额准入、再直连业务服」的时序。

## Considered Options

- **票据保留 character_id,允许 0 表示未选**:被否。票据语义出现两种形态,Gateway 仍需读角色,Realm 要同时处理「票据已定角色」与「入场后选角」两条路径。
- **角色管理放在 Realm 之外(如 Login Verifier 的 HTTPS 接口)**:被否。角色是 Realm 内的游戏身份,放到登录链前段会让账号验证服承载业务写入。
- **删除 Gateway 的 Fetching 阶段**:被否。Fetching 仍以限额 majority 读复核封禁与白名单,它是浪涌下的准入节流与 M1–M4 验收的测量点。

## Consequences

- 1007 收窄为「封禁或白名单外」,不再包含「无选定角色」。
- 票据格式直接修改、用途值 3(EnterRealm)不变,网关与 Realm 同版本发布,不保留双格式;旧票据在有效期内自然失效。
- 角色归属复核从入场兑换移到选角与游戏动作的处理时点。
