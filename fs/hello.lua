-- hello.lua — a smoke-test script for the AdventOS lua interpreter.
-- Demonstrates the major working pieces: variables, control flow,
-- tables, functions, recursion, string ops.

print("hello from advent lua")

-- Arithmetic + control flow.
local sum = 0
for i = 1, 10 do
    sum = sum + i
end
print("sum 1..10 =", sum)

-- Recursion + locals.
local function fact(n)
    if n <= 1 then return 1 end
    return n * fact(n - 1)
end
print("10! =", fact(10))

-- Tables: array and hash mixed.
local t = { "apple", "banana", "cherry" }
table.insert(t, "date")
print("table length:", #t)
for i = 1, #t do
    print("  " .. i .. ": " .. t[i])
end

print("joined:", table.concat(t, ", "))

-- String operations.
local greeting = "AdventOS"
print(string.upper(greeting) .. "!")
print("length:", string.len(greeting))
print("first 6:", string.sub(greeting, 1, 6))

-- if/elseif/else chain.
local function classify(n)
    if n < 0 then return "negative"
    elseif n == 0 then return "zero"
    elseif n < 10 then return "small"
    else return "large"
    end
end
print(classify(-3))
print(classify(0))
print(classify(5))
print(classify(100))
