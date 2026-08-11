import os, re
p = 'kernel/core/ipc.cpp'
with open(p, 'rb') as f: data = f.read()
try: text = data.decode('gbk')
except: text = data.decode('utf-16le', errors='replace')
# Remove enqueue/dequeue functions
text = re.sub(r'void Endpoint::enqueue_sender.*?TaskControlBlock\* Endpoint::dequeue_receiver\(\) \{.*?return tcb;\s*\}\s*', '', text, flags=re.DOTALL)
# Replace uses
text = text.replace('recv_head_', '!recv_queue_.empty()')
text = text.replace('dequeue_receiver()', 'recv_queue_.dequeue()')
text = text.replace('enqueue_sender(sender)', 'send_queue_.enqueue(sender)')
text = text.replace('send_head_', '!send_queue_.empty()')
text = text.replace('dequeue_sender()', 'send_queue_.dequeue()')
text = text.replace('enqueue_receiver(receiver)', 'recv_queue_.enqueue(receiver)')
with open(p, 'w', encoding='utf-8') as f: f.write(text)
