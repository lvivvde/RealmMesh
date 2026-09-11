-- 测试基座自检:验证 realm_lua_cli 解释器 + luaunit 注入路径可用。
-- 业务 Lua 模块的套件照本文件的结构编写:定义 TestXxx 表,文件尾
-- os.exit(lu.LuaUnit.run()) 结尾(退出码 = 失败+错误数,0 即通过)。
local lu = require("luaunit")

TestHarness = {}

function TestHarness:testRunsOnVendoredLua54()
    lu.assertEquals(_VERSION, "Lua 5.4")
end

function TestHarness:testLuaunitAssertionsWork()
    lu.assertEquals(1 + 1, 2)
    lu.assertNotEquals("a", "b")
    lu.assertIsTrue(1 < 2)
    lu.assertStrContains("realmmesh", "mesh")
end

function TestHarness:testTablesCompareDeeply()
    lu.assertEquals({ a = 1, b = { c = 2 } }, { a = 1, b = { c = 2 } })
end

os.exit(lu.LuaUnit.run())
