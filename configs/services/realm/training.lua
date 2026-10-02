-- Realm 训练规则:纯函数模块,Realm 启动时经 realm.training_rule_file 加载,
-- 在帧线程上调用;不做 IO,不热更。
-- 每次训练 +10 经验;等级 = exp // 100 + 1,10 级(经验 900)封顶。

local EXP_PER_TRAINING = 10
local EXP_PER_LEVEL = 100
local MAX_LEVEL = 10
local MAX_LEVEL_EXP = (MAX_LEVEL - 1) * EXP_PER_LEVEL

local rule = {}

-- 由经验算等级;满级后经验不再增长,等级同样封顶。
function rule.level(exp)
    return math.min(exp // EXP_PER_LEVEL + 1, MAX_LEVEL)
end

-- 训练一次后的经验;已满级返回 nil(Realm 回 3008)。
function rule.train(exp)
    if exp >= MAX_LEVEL_EXP then return nil end
    return exp + EXP_PER_TRAINING
end

return rule
