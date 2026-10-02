-- 训练规则(configs/services/realm/training.lua)的纯函数套件:
-- 每次 +10 经验,等级 = exp // 100 + 1,封顶 10 级(经验 900),满级拒绝。
local lu = require("luaunit")

local repository_root = arg[0]:match("^(.*)/tests/lua/realm/[^/]+$")
assert(repository_root, "cannot derive repository root from " .. arg[0])
local rule = dofile(repository_root .. "/configs/services/realm/training.lua")

TestTrainingLevel = {}

function TestTrainingLevel:testNewCharacterIsLevelOne()
    lu.assertEquals(rule.level(0), 1)
end

function TestTrainingLevel:testLevelStepsEveryHundredExp()
    lu.assertEquals(rule.level(99), 1)
    lu.assertEquals(rule.level(100), 2)
    lu.assertEquals(rule.level(250), 3)
    lu.assertEquals(rule.level(899), 9)
end

function TestTrainingLevel:testLevelCapsAtTen()
    lu.assertEquals(rule.level(900), 10)
    lu.assertEquals(rule.level(5000), 10)
end

function TestTrainingLevel:testLevelIsAnInteger()
    lu.assertEquals(math.type(rule.level(250)), "integer")
end

TestTrain = {}

function TestTrain:testAddsTenExp()
    lu.assertEquals(rule.train(0), 10)
    lu.assertEquals(rule.train(30), 40)
end

function TestTrain:testCrossesLevelBoundary()
    local exp = rule.train(90)
    lu.assertEquals(exp, 100)
    lu.assertEquals(rule.level(exp), 2)
end

function TestTrain:testReachesMaxLevelExactly()
    local exp = rule.train(890)
    lu.assertEquals(exp, 900)
    lu.assertEquals(rule.level(exp), 10)
end

function TestTrain:testRejectsAtMaxLevel()
    lu.assertNil(rule.train(900))
end

function TestTrain:testNinetyTrainingsFromZeroReachMaxLevel()
    local exp, count = 0, 0
    while true do
        local next_exp = rule.train(exp)
        if next_exp == nil then break end
        exp, count = next_exp, count + 1
    end
    lu.assertEquals(count, 90)
    lu.assertEquals(exp, 900)
end

function TestTrain:testResultIsAnInteger()
    lu.assertEquals(math.type(rule.train(0)), "integer")
end

os.exit(lu.LuaUnit.run())
