import os
base = os.path.dirname(os.path.abspath(__file__))
web = os.path.join(base, '..', 'Web', 'index.html')
with open(web, 'r', encoding='utf-8') as f:
    h = f.read()

old = "if(acd){if(acd.ct)t+=' · '+(acd.mA/1000).toFixed(3)+' A';}"
new = "if(acd){if(acd.ct)t+=' · '+(acd.mA/1000).toFixed(3)+' A';else t+=' · CT 未就緒';}"
if old in h:
    h = h.replace(old, new)
    print('Fixed: added CT 未就緒 fallback')
else:
    print('Pattern not found. Looking for alternatives...')
    # Try finding the fragment
    for s in ["if(acd){if(acd.ct)", "acd.ct)t+=", "toFixed(3)+' A'"]:
        if s in h:
            print(f'  Found: {s}')
        else:
            print(f'  NOT found: {s}')

with open(web, 'w', encoding='utf-8') as f:
    f.write(h)
print('Done')
