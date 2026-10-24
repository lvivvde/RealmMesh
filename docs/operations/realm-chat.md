# Realm 与参考客户端 Chat 接入（C3）

本页对应 [#178](https://github.com/lvivvde/RealmMesh/issues/178)，基于
[Chat 规格](../specs/2026-10-23-chat-reference-loop.md)和[独立服务说明](chat-service.md)。
单 Chat 闭环已接线；双 Scene/双 Chat 的转场、真实慢消费者积压及组合故障总验收由 C4–C5 完成。

## 配置

默认 `configs/services/realm.lua` 的 `realm.chat.enabled=false`；四服务登录拓扑不依赖 Chat。
启用时提供服务 CA、Realm 证书/私钥（仓库外环境变量），在 `targets` 中列出至多两个 Chat：
`instance_id`、精确 `server_identity`、后备 `host/port` 与 `channels`。频道编号 0 表示 Realm
Channel，1/2 表示对应 Scene Channel；同频道不得配置两个承载，不自动换主。

开启宿主服务发现后，连接 IO 从 etcd 查配置指定的精确实例，缺失只使该 Chat 不可用；配置的
CA/SAN/HELLO 仍是认证边界。发现关闭时使用静态端点。缺少认证文件不阻止 Realm 启动，但 Chat
加入失败。配置自身形状错误（重复承载、越界频道）仍拒绝。手动运行涉及玩家数据时遵循
[共享 MongoDB 指引](shared-mongodb.md)，通过 `scripts/with-shared-mongodb.sh`，不打印认证材料。

## 客户端

在 Login Chain 返回的 Realm Session 上取 `chat()`。未选角请求回3202，非法Chat请求回3201；
选角后 Realm Channel 异步加入，Scene Channel 另需成功 EnterScene/激活。先 GetChatState 取得
ACTIVE、资格代次、Epoch 和原加入水位 W；发送时显式填入这些字段与独立 client_send_seq，首号1。
成功回执说明在线受理。随后 `poll_chat` 消费通知（包括发送者本人），回执不代替通知。

一次只执行一个客户端调用。Scene、训练、心跳和Chat共享原Realm连接，推送分类保留；初次
取ChatState前的消息暂存并在取得W后过滤。重复状态查询不清空通知水位。缺口显式通知不可用，
重新加入从新W继续，没有历史补投。缓存满关闭Realm Session，重新登录取得新资格。

Unknown 只能查询最后回执，或在相同有效资格/Epoch下用原号、原文显式重试；本库不自动换号、
换资格或跨Epoch发送。即使旧资格重试明确被拒，原Unknown也不能被解释成原消息未受理。

## 资源、故障与验证

每承载独立TLS IO，最多8项排队、64项运行/排队/未取完成，单次总截止1秒；剩余窗口至多原请求
重试一次。自动加入最多3次，之后显式状态查询触发下一轮。Realm全局按角色跨频道合计5条/秒、
突发10；每角色两个资格（旧撤销/未收完成仍占位）。角色顶替等待旧资格结算，不能同时有第三资格。
Realm及参考客户端的Chat缓存均为256项/256KiB；Scene客户端缓存也有数量与字节上限。

`RealmChatTest` 使用真实mTLS边界与生产ChatChannels验证丢JOIN夹入W+1、丢SEND回执只分发一次、
Unknown、顶替与异步失败。`ChatSessionTest` 验证初次状态前消息、分类保留、重复/缺口/缓存满及
Unknown重试。`ChatJourneyTest` 拉起真实登录服务、Realm、Chat、Scene、etcd和隔离MongoDB，
验证双人连续顺序、角色资格拒绝、训练/心跳/移动共存、数据库落点、HTTP指标、SIGKILL及同名重启。
重启前等待旧etcd注册实际过期。夹具回收后核对服务端口；自动测试不接触共享开发库。

定向运行构建相应目标后：

```sh
ctest --preset dev -j 1 -R '^(RealmChatTest|ChatSessionTest|ChatJourneyTest)\.'
```

阶段完成仍需双平台当前源码 `./scripts/build.sh` 全量；定向测试不代替完整回归。C3不宣称
C4/C5的双承载隔离、跨分片转场或真实积压饱和已经验收。
