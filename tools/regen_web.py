import os
base = os.path.dirname(os.path.abspath(__file__))
web = os.path.join(base, '..', 'Web', 'index.html')
out = os.path.join(base, '..', 'Web', 'web_assets.h')
with open(web,'rb') as f: data=f.read()
lines=['#ifndef WEB_ASSETS_H','#define WEB_ASSETS_H','','#define WEB_INDEX_HTML_LEN {}U'.format(len(data)),'','static const unsigned char WEB_INDEX_HTML[] = {']
for i in range(0,len(data),16):
    chunk=data[i:i+16];h=','.join('0x{:02X}U'.format(b) for b in chunk)
    lines.append('  '+h+(',' if i+16<len(data) else ''))
lines+=['};','','#endif']
with open(out,'w',newline='\n') as f: f.write('\n'.join(lines)+'\n')
print('OK:',len(data),'bytes')
