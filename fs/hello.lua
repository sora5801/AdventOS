-- hello.lua — smoke-test for the AdventOS lua interpreter.
-- Exercises every major feature the interpreter ships, organized
-- by session for tracing.

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

-- Tables: positional + named, length, indexing, concat.
local t = { "apple", "banana", "cherry" }
table.insert(t, "date")
print("table length:", #t)
print("joined:", table.concat(t, ", "))

-- ===== Session 88: pcall + error =====

local ok = pcall(function()
    error("intentional failure")
end)
print("pcall caught:", ok, "msg:", last_error())

-- pcall around a no-error function returns the value through.
print("pcall ok:", pcall(function() return 42 end))

-- ===== Session 88: closures with upvalue capture =====
-- Capture-by-value, so a closure sees the outer locals at creation
-- time. Subsequent changes in the outer scope aren't visible — this
-- is a documented difference from real Lua.

local function make_greeter(name)
    return function() return "hello " .. name end
end
local g1 = make_greeter("world")
local g2 = make_greeter("AdventOS")
print(g1())
print(g2())

-- ===== Session 88: string.find / string.byte / string.char =====

print("'AdventOS' contains 'Os':", string.find("AdventOS", "Os"))
print("'hello' first byte:", string.byte("hello", 1))    -- 104
print("byte 104 as char:", string.char(104))             -- "h"

-- ===== Session 88: GC =====
-- Allocate a bunch and force a collection. Should not crash.

for i = 1, 500 do
    local junk = { "a", "b", "c", "d" }
    junk[5] = string.rep("x", 16)
end
collectgarbage()
print("gc survived 500 alloc cycles")
