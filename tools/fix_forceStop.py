import os
base = os.path.dirname(os.path.abspath(__file__))
web = os.path.join(base, '..', 'Web', 'index.html')
with open(web, 'r', encoding='utf-8') as f:
    h = f.read()

# Fix 1: truncated forceStopRelay
old = "function forceStopRelay(channel){if(!isMaster()){showMessage('只有 Master 可以控制本地繼電器。','warn');return}"
if old in h:
    new = old + "const d=await apiFetch('/api/protection/force-stop',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'channel='+channel});if(!d.ok){showMessage('強制停止失敗：'+d.error,'error');return}showMessage('通道 '+channel+' 已強制停止，冷卻 3 秒。');await new Promise(r=>setTimeout(r,500));await refreshAll(true,'manual')}"
    h = h.replace(old, new)
    print('Fixed forceStopRelay')
else:
    # Find what's actually there
    i = h.find('function forceStopRelay')
    if i >= 0:
        print('Found at', i, ':', repr(h[i:i+200]))
    else:
        print('forceStopRelay NOT FOUND at all!')

# Fix 2: truncated forceStopRemoteRelay
old2 = "function forceStopRemoteRelay(nodeId,channel){if(!isMaster()){showMessage('只有 Master 可以控制遠端繼電器。','warn');return}"
if old2 in h:
    new2 = old2 + "const node=state.nodes.find(n=>Number(n.node_id)===Number(nodeId));if(!node||!node.ip_text){showMessage('找不到遠端節點。','error');return}const d=await remoteApiFetch(node.ip_text,'/api/protection/force-stop',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'channel='+channel});if(!d.ok){showMessage('遠端強制停止失敗：'+d.error,'error');return}showMessage('遠端通道 '+channel+' 已強制停止。');await new Promise(r=>setTimeout(r,500));await refreshAll(true,'manual')}"
    h = h.replace(old2, new2)
    print('Fixed forceStopRemoteRelay')

# Check brace balance
opens = h.count('{')
closes = h.count('}')
print(f'Braces: {{ = {opens}, }} = {closes}, diff = {opens-closes}')

with open(web, 'w', encoding='utf-8') as f:
    f.write(h)
print('Done')
