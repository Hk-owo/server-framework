#!/bin/bash

BASE_URL="http://localhost:8080"

echo "========================================="
echo " Low Pressure Benchmark"
echo "========================================="

echo ""
echo "▶ /hello"
wrk -t2 -c10 -d10s "$BASE_URL/hello?name=Benchmark"

echo ""
echo "▶ /json"
wrk -t2 -c10 -d10s "$BASE_URL/json"

echo ""
echo "========================================="
echo " Benchmark Complete"
echo "========================================="
