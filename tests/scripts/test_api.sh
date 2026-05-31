#!/bin/bash

BASE_URL="http://localhost:8080"

echo "========================================="
echo " HTTP Server Framework API Test"
echo "========================================="

# 1. 测试 /hello
echo ""
echo "1. Testing /hello..."
RESP=$(curl -s "$BASE_URL/hello?name=Framework" --max-time 5)
echo "Response: $RESP"
if echo "$RESP" | grep -q "Hello, Framework"; then
    echo "✅ /hello OK"
else
    echo "❌ /hello failed"
fi

# 2. 测试 /echo
echo ""
echo "2. Testing /echo..."
RESP=$(curl -s -X POST "$BASE_URL/echo" -d "ping" --max-time 5)
echo "Response: $RESP"
if [ "$RESP" = "ping" ]; then
    echo "✅ /echo OK"
else
    echo "❌ /echo failed"
fi

# 3. 测试 /json
echo ""
echo "3. Testing /json..."
RESP=$(curl -s "$BASE_URL/json" --max-time 5)
echo "Response: ${RESP:0:200}..."
if echo "$RESP" | grep -q '"status":"ok"'; then
    echo "✅ /json OK"
else
    echo "❌ /json failed"
fi

# 4. 测试 /api/status
echo ""
echo "4. Testing /api/status..."
RESP=$(curl -s "$BASE_URL/api/status" --max-time 5)
echo "Response: ${RESP:0:200}..."
if echo "$RESP" | grep -q '"success":true'; then
    echo "✅ /api/status OK"
else
    echo "❌ /api/status failed"
fi

# 5. 测试 /api/user/{id}
echo ""
echo "5. Testing /api/user/42..."
RESP=$(curl -s "$BASE_URL/api/user/42" --max-time 5)
echo "Response: $RESP"
if echo "$RESP" | grep -q '"userId":"42"'; then
    echo "✅ /api/user/:id OK"
else
    echo "❌ /api/user/:id failed"
fi

echo ""
echo "========================================="
echo " Simple Test Complete"
echo "========================================="
