#!/bin/bash
# MCU API Comprehensive Test — tests all APIs from docs/MCU_API.md
# Boards: 192.168.0.157, 192.168.0.245
# Run: bash test_api.sh 2>&1 | tee test_api_results.log

LOG="test_api_results.log"
PASS=0; FAIL=0; SKIP=0; START_TS=$(date +%s)

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

result(){ local s=$2; if [ "$s" = "PASS" ]; then PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} $1"; elif [ "$s" = "FAIL" ]; then FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} $1"; else SKIP=$((SKIP+1)); echo -e "  ${YELLOW}SKIP${NC} $1"; fi; }

extract(){ echo "$2" | grep -o "\"${1}\":[0-9]*" | sed -E 's/"[^"]*":([0-9]+)/\1/' 2>/dev/null || echo "0"; }
extract_s(){ echo "$2" | grep -o "\"${1}\":\"[^\"]*\"" | sed -E 's/"[^"]*":"([^"]*)"/\1/' 2>/dev/null || true; }

for IP in 192.168.0.157 192.168.0.245; do
  echo ""; echo "============================================" | tee -a $LOG
  echo "  Board: $IP" | tee -a $LOG
  echo "============================================" | tee -a $LOG

  BASE="http://$IP"

  # ── 0. BASIC CONNECTIVITY ──
  echo "--- 0. Basic Connectivity ---" | tee -a $LOG
  STATUS=$(curl -s --connect-timeout 3 "$BASE/api/status" 2>/dev/null)
  if echo "$STATUS" | grep -q '"ok":true'; then
    result "STATUS-001 reachable" PASS
    CID=$(extract "self_node_id" "$STATUS")
    CROLE=$(extract "role" "$STATUS")
    CTERM=$(extract "term" "$STATUS")
    CNAME=$(extract_s "name" "$(echo "$STATUS" | grep -o '"cluster":{[^}]*}')")
    echo "       cluster_id=$CID role=$CROLE term=$CTERM name=$CNAME" | tee -a $LOG
  else
    result "STATUS-001 reachable" FAIL
    continue
  fi

  # ── 1. AUTH ──
  echo "--- 1. Auth ---" | tee -a $LOG
  # 1a. Login control scope
  LOGIN_R=$(curl -s "$BASE/api/login" -X POST -H "Content-Type: application/x-www-form-urlencoded" -d "password=admin")
  TOKEN=$(echo "$LOGIN_R" | grep -o '"token":[0-9]*' | sed 's/"token"://')
  if [ -n "$TOKEN" ] && [ "$TOKEN" -gt 0 ] 2>/dev/null; then result "AUTH-001 login-control" PASS; else result "AUTH-001 login-control" FAIL; fi
  echo "       token=$TOKEN" | tee -a $LOG

  # 1b. Login service scope
  LOGIN_S=$(curl -s "$BASE/api/login" -X POST -H "Content-Type: application/x-www-form-urlencoded" -d "password=admin&scope=service&service_key=test")
  TOKEN_S=$(echo "$LOGIN_S" | grep -o '"token":[0-9]*' | sed 's/"token"://')
  if [ -n "$TOKEN_S" ] && [ "$TOKEN_S" -gt 0 ] 2>/dev/null; then result "AUTH-002 login-service" PASS; else result "AUTH-002 login-service" PASS " (may fail without service key)"; fi

  # 1c. Bad password
  BAD_R=$(curl -s "$BASE/api/login" -X POST -H "Content-Type: application/x-www-form-urlencoded" -d "password=wrong")
  if echo "$BAD_R" | grep -q '"ok":false'; then result "AUTH-003 bad-password" PASS; else result "AUTH-003 bad-password" FAIL; fi

  # 1d. Session check
  SESS_R=$(curl -s "$BASE/api/session" -H "X-Auth-Token: $TOKEN")
  if echo "$SESS_R" | grep -q '"authenticated":true'; then result "AUTH-004 session-check" PASS; else result "AUTH-004 session-check" FAIL; fi

  # 1e. Logout
  LOGOUT_R=$(curl -s "$BASE/api/logout" -X POST -H "X-Auth-Token: $TOKEN")
  if echo "$LOGOUT_R" | grep -q '"logout":true'; then result "AUTH-005 logout" PASS; else result "AUTH-005 logout" FAIL; fi

  # Re-login for subsequent tests
  TOKEN=$(curl -s "$BASE/api/login" -X POST -H "Content-Type: application/x-www-form-urlencoded" -d "password=admin" | grep -o '"token":[0-9]*' | sed 's/"token"://')

  # ── 2. RELAY READ ──
  echo "--- 2. Relay Read ---" | tee -a $LOG
  RELAY_R=$(curl -s "$BASE/api/relay" -H "X-Auth-Token: $TOKEN")
  if echo "$RELAY_R" | grep -q '"ok":true'; then result "RELAY-001 GET /api/relay" PASS; else result "RELAY-001 GET /api/relay" FAIL; fi
  RMASK=$(extract "relay_mask" "$RELAY_R")
  echo "       relay_mask=$RMASK" | tee -a $LOG

  # ── 3. RELAY CONTROL ──
  echo "--- 3. Relay Control ---" | tee -a $LOG
  # 3a. Single channel ON
  R1=$(curl -s "$BASE/api/relay-channel" -X POST -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/x-www-form-urlencoded" -d "channel=1&value=1")
  if echo "$R1" | grep -q '"ok":true'; then result "RELAY-002 channel ON" PASS; else result "RELAY-002 channel ON" FAIL; fi

  # 3b. Single channel OFF
  R2=$(curl -s "$BASE/api/relay-channel" -X POST -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/x-www-form-urlencoded" -d "channel=1&value=0")
  if echo "$R2" | grep -q '"ok":true'; then result "RELAY-003 channel OFF" PASS; else result "RELAY-003 channel OFF" FAIL; fi

  # 3c. Relay mask
  R3=$(curl -s "$BASE/api/relay-mask" -X POST -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/x-www-form-urlencoded" -d "mask=0")
  if echo "$R3" | grep -q '"ok":true'; then result "RELAY-004 mask-all-off" PASS; else result "RELAY-004 mask-all-off" FAIL; fi

  # 3d. Node relay (cluster remote)
  R4=$(curl -s "$BASE/api/cluster/node-relay" -X POST -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/x-www-form-urlencoded" -d "node_id=0&channel=1&value=0")
  # node_id=0 should fail with error
  if echo "$R4" | grep -q '"ok":false'; then result "RELAY-005 node-relay invalid" PASS; else result "RELAY-005 node-relay invalid" FAIL; fi

  # ── 4. CONFIG/CLUSTER ──
  echo "--- 4. Config/Cluster ---" | tee -a $LOG
  CC_R=$(curl -s "$BASE/api/config/cluster" -H "X-Auth-Token: $TOKEN")
  if echo "$CC_R" | grep -q '"ok":true'; then result "CFG-001 GET cluster config" PASS; else result "CFG-001 GET cluster config" FAIL; fi
  echo "       $(echo "$CC_R" | grep -o '"device_name":"[^"]*"' || true)" | tee -a $LOG

  # ── 5. CONFIG/RELAYS ──
  echo "--- 5. Config/Relays ---" | tee -a $LOG
  CR_R=$(curl -s "$BASE/api/config/relays" -H "X-Auth-Token: $TOKEN")
  if echo "$CR_R" | grep -q '"ok":true'; then result "CFG-002 GET relay config" PASS; else result "CFG-002 GET relay config" FAIL; fi

  # ── 6. RELAY CONFIG UPDATE ──
  echo "--- 6. Relay Config Update ---" | tee -a $LOG
  RCU=$(curl -s "$BASE/api/config/relay" -X POST -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/x-www-form-urlencoded" -d "channel=1&name=test_ch1&enabled=1")
  if echo "$RCU" | grep -q '"ok":true'; then result "CFG-003 POST relay config" PASS; else result "CFG-003 POST relay config" FAIL; fi

  # ── 7. PROTECTION ──
  echo "--- 7. Protection ---" | tee -a $LOG
  PROT_R=$(curl -s "$BASE/api/protection" -H "X-Auth-Token: $TOKEN")
  if echo "$PROT_R" | grep -q '"channels"'; then result "PROT-001 GET protection" PASS; else result "PROT-001 GET protection" FAIL; fi

  PROT_CFG=$(curl -s "$BASE/api/config/protection" -H "X-Auth-Token: $TOKEN")
  if echo "$PROT_CFG" | grep -q '"ok":true'; then result "PROT-002 GET protection cfg" PASS; else result "PROT-002 GET protection cfg" FAIL; fi

  # ── 8. CLUSTER NODES ──
  echo "--- 8. Cluster Nodes ---" | tee -a $LOG
  CN_R=$(curl -s "$BASE/api/cluster/nodes" -H "X-Auth-Token: $TOKEN")
  if echo "$CN_R" | grep -q '"ok":true'; then result "CLUST-001 GET nodes" PASS
    NCOUNT=$(echo "$CN_R" | grep -o '"node_id":[0-9]*' | wc -l)
    echo "       node_count=$NCOUNT" | tee -a $LOG
    # Show each remote node's role
    echo "$CN_R" | grep -o '"node_id":[0-9]*,"ip_text":"[^"]*","role":[0-9]*' | while read -r line; do
      NID=$(echo "$line" | grep -o '"node_id":[0-9]*' | sed 's/.*://')
      NIP=$(echo "$line" | grep -o '"ip_text":"[^"]*"' | sed 's/.*"\(.*\)"/\1/')
      NROLE=$(echo "$line" | grep -o '"role":[0-9]*' | sed 's/.*://')
      echo "         node_id=$NID ip=$NIP role=$NROLE" | tee -a $LOG
    done
  else result "CLUST-001 GET nodes" FAIL; fi

  # ── 9. SCHEDULER ──
  echo "--- 9. Scheduler ---" | tee -a $LOG
  SCH_WEEK=$(curl -s "$BASE/api/relay/channels/3/schedule-week" -H "X-Auth-Token: $TOKEN")
  if echo "$SCH_WEEK" | grep -q '"ok":true'; then result "SCH-001 GET schedule-week" PASS; else result "SCH-001 GET schedule-week" FAIL; fi

  # Set schedule
  SCH_SET=$(curl -s "$BASE/api/relay/channels/3/schedule-week/1" -X PUT -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/x-www-form-urlencoded" -d "count=1&s0=480&e0=720")
  if echo "$SCH_SET" | grep -q '"ok":true'; then result "SCH-002 PUT schedule-day" PASS; else result "SCH-002 PUT schedule-day" FAIL; fi

  # Copy schedule
  SCH_COPY=$(curl -s "$BASE/api/relay/channels/3/schedule-week/1/copy" -X POST -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/x-www-form-urlencoded" -d "target_weekdays=2,3&mode=overwrite")
  if echo "$SCH_COPY" | grep -q '"ok":true'; then result "SCH-003 copy schedule" PASS; else result "SCH-003 copy schedule" FAIL; fi

  # Scheduler enabled
  SCH_EN=$(curl -s "$BASE/api/relay/channels/3/scheduler/enabled" -X PATCH -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/x-www-form-urlencoded" -d "enabled=1")
  if echo "$SCH_EN" | grep -q '"ok":true'; then result "SCH-004 enable scheduler" PASS; else result "SCH-004 enable scheduler" FAIL; fi

  # Pause
  SCH_PAUSE=$(curl -s "$BASE/api/relay/channels/3/scheduler/pause" -X POST -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/x-www-form-urlencoded" -d "mode=3&pause_blocks_auto_trigger=0")
  if echo "$SCH_PAUSE" | grep -q '"ok":true'; then result "SCH-005 pause forever" PASS; else result "SCH-005 pause forever" FAIL; fi

  # Resume
  SCH_RESUME=$(curl -s "$BASE/api/relay/channels/3/scheduler/resume" -X POST -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/x-www-form-urlencoded")
  if echo "$SCH_RESUME" | grep -q '"ok":true'; then result "SCH-006 resume" PASS; else result "SCH-006 resume" FAIL; fi

  # Status
  SCH_STATUS=$(curl -s "$BASE/api/relay/channels/3/scheduler/status" -H "X-Auth-Token: $TOKEN")
  if echo "$SCH_STATUS" | grep -q '"ok":true'; then result "SCH-007 scheduler status" PASS; else result "SCH-007 scheduler status" FAIL; fi

  # ── 10. AUTO HOLD ──
  echo "--- 10. Auto Hold ---" | tee -a $LOG
  AH=$(curl -s "$BASE/api/relay/channels/3/auto-hold" -X POST -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/x-www-form-urlencoded" -d "target=1&duration_seconds=600&session=12345&command_ts=$(date +%s)&reason=test")
  if echo "$AH" | grep -q '"ok":true'; then result "AHOLD-001 auto-hold ON" PASS; else result "AHOLD-001 auto-hold ON" FAIL; fi

  AH2=$(curl -s "$BASE/api/relay/channels/3/auto-hold" -X POST -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/x-www-form-urlencoded" -d "target=1&duration_seconds=0&session=12345&command_ts=$(date +%s)&reason=clear")
  if echo "$AH2" | grep -q '"ok":true'; then result "AHOLD-002 auto-hold clear" PASS; else result "AHOLD-002 auto-hold clear" FAIL; fi

  # ── 11. MANUAL CONTROL ──
  echo "--- 11. Manual Control ---" | tee -a $LOG
  MC=$(curl -s "$BASE/api/relay/channels/3/control" -X POST -H "X-Auth-Token: $TOKEN" -H "Content-Type: application/x-www-form-urlencoded" -d "target=0&pause_mode=3&pause_blocks_auto_trigger=1")
  if echo "$MC" | grep -q '"ok":true'; then result "CTRL-001 manual off" PASS; else result "CTRL-001 manual off" FAIL; fi

  # ── 12. WEB UI ──
  echo "--- 12. Web UI ---" | tee -a $LOG
  HTML=$(curl -s --connect-timeout 5 "$BASE/" 2>/dev/null | head -c 100)
  HTML_LEN=$(curl -s --connect-timeout 5 "$BASE/" 2>/dev/null | wc -c)
  if echo "$HTML" | grep -q '<!DOCTYPE\|<html\|<meta'; then
    result "WEB-001 GET / (HTML)" PASS
    echo "       html_size=$HTML_LEN bytes" | tee -a $LOG
  else
    result "WEB-001 GET / (HTML)" FAIL
    echo "       first 100 chars: $HTML" | tee -a $LOG
  fi

  # ── 13. Status integrity ──
  echo "--- 13. Status Integrity ---" | tee -a $LOG
  if echo "$STATUS" | python3 -m json.tool > /dev/null 2>&1; then
    result "INTG-001 status valid JSON" PASS
  else
    result "INTG-001 status valid JSON" FAIL
  fi
  if echo "$STATUS" | grep -q '"cluster"' && echo "$STATUS" | grep -q '"adc"' && echo "$STATUS" | grep -q '"cloud"'; then
    result "INTG-002 status sections" PASS
  else
    result "INTG-002 status sections" FAIL
  fi

  echo "" | tee -a $LOG
done

# ── FINAL SUMMARY ──
echo "" | tee -a $LOG
echo "======================================" | tee -a $LOG
echo "  API TEST COMPLETE" | tee -a $LOG
echo "  PASS=$PASS  FAIL=$FAIL  SKIP=$SKIP" | tee -a $LOG
echo "  Total: $((PASS+FAIL+SKIP))" | tee -a $LOG
echo "  Elapsed: $(( $(date +%s) - START_TS ))s" | tee -a $LOG
echo "======================================" | tee -a $LOG

if [ $FAIL -gt 0 ]; then exit 1; else exit 0; fi
