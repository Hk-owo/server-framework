#!/bin/bash

BASE_URL="http://localhost:8080"

echo "========================================="
echo " Progressive Stress Test"
echo "========================================="

TEST_CASES=(
    "2,10,10s,Low(2t/10c)"
    "2,50,10s,Medium(2t/50c)"
    "4,100,10s,High(4t/100c)"
    "4,200,10s,VeryHigh(4t/200c)"
    "8,500,10s,Extreme(8t/500c)"
)

for test_case in "${TEST_CASES[@]}"; do
    IFS=',' read -r threads connections duration desc <<< "$test_case"
    echo ""
    echo "#########################################"
    echo " Level: $desc"
    echo "#########################################"
    timeout 60 wrk -t"$threads" -c"$connections" -d"$duration" --latency "$BASE_URL/json" 2>&1 || echo "⚠️ timeout or failed"
    sleep 2
done

echo ""
echo "========================================="
echo " Stress Test Complete"
echo "========================================="
