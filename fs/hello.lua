-- hello.lua — smoke-test for the AdventOS lua interpreter.
-- Organized by session for tracing.

-- ===== Session 87: core types, control flow, recursion =====

print("hello from advent lua")

local sum = 0
for i = 1, 10 do sum = sum + i end
print("sum 1..10 =", sum)

local function fact(n)
    if n <= 1 then return 1 end
    return n * fact(n - 1)
end
print("10! =", fact(10))

local t = { "apple", "banana", "cherry" }
table.insert(t, "date")
print("table length:", #t)
print("joined:", table.concat(t, ", "))

-- ===== Session 88: pcall + closures + GC + string ops =====

local ok, msg = pcall(function() error("intentional failure") end)
print("pcall caught:", ok, "msg:", msg)
print("pcall ok:",     pcall(function() return 42 end))

local function make_greeter(name)
    return function() return "hello " .. name end
end
print(make_greeter("world")())
print(make_greeter("AdventOS")())

print("'AdventOS' contains 'Os' at:", string.find("AdventOS", "Os"))
print("'hello' byte 1:", string.byte("hello", 1))
print("char 104:", string.char(104))

-- ===== Session 89: multi-return + generic for =====

-- Multi-return from a function.
local function pair_of(a, b) return a, b end
local x, y = pair_of(10, 20)
print("multi-return: x=" .. x .. " y=" .. y)

-- Swap via multi-assignment.
local p, q = 1, 2
p, q = q, p
print("swap: p=" .. p .. " q=" .. q)

-- Generic for with ipairs (positional iteration).
print("ipairs walk:")
for i, v in ipairs(t) do
    print("  [" .. i .. "] = " .. v)
end

-- Generic for with pairs (full table iteration).
local config = { name = "AdventOS", version = 1, smp = 2 }
print("pairs walk:")
for k, v in pairs(config) do
    print("  " .. tostring(k) .. " => " .. tostring(v))
end

-- pcall with real multi-return on success.
local ok2, a, b, c = pcall(function() return 1, 2, 3 end)
print("pcall returns:", ok2, a, b, c)

-- ===== GC stress =====
for i = 1, 500 do
    local junk = { "a", "b", "c", "d" }
    junk[5] = string.rep("x", 16)
end
collectgarbage()
print("gc survived 500 alloc cycles")
